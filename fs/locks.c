// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/locks.c
 *
 * We implement four types of file locks: BSD locks, posix locks, open
 * file description locks, and leases.  For details about BSD locks,
 * see the flock(2) man page; for details about the other three, see
 * fcntl(2).
 *
 *
 * Locking conflicts and dependencies:
 * If multiple threads attempt to lock the same byte (or flock the same file)
 * only one can be granted the lock, and other must wait their turn.
 * The first lock has been "applied" or "granted", the others are "waiting"
 * and are "blocked" by the "applied" lock..
 *
 * Waiting and applied locks are all kept in trees whose properties are:
 *
 *	- the root of a tree may be an applied or waiting lock.
 *	- every other node in the tree is a waiting lock that
 *	  conflicts with every ancestor of that node.
 *
 * Every such tree begins life as a waiting singleton which obviously
 * satisfies the above properties.
 *
 * The only ways we modify trees preserve these properties:
 *
 *	1. We may add a new leaf node, but only after first verifying that it
 *	   conflicts with all of its ancestors.
 *	2. We may remove the root of a tree, creating a new singleton
 *	   tree from the root and N new trees rooted in the immediate
 *	   children.
 *	3. If the root of a tree is not currently an applied lock, we may
 *	   apply it (if possible).
 *	4. We may upgrade the root of the tree (either extend its range,
 *	   or upgrade its entire range from read to write).
 *
 * When an applied lock is modified in a way that reduces or downgrades any
 * part of its range, we remove all its children (2 above).  This particularly
 * happens when a lock is unlocked.
 *
 * For each of those child trees we "wake up" the thread which is
 * waiting for the lock so it can continue handling as follows: if the
 * root of the tree applies, we do so (3).  If it doesn't, it must
 * conflict with some applied lock.  We remove (wake up) all of its children
 * (2), and add it is a new leaf to the tree rooted in the applied
 * lock (1).  We then repeat the process recursively with those
 * children.
 *
 */
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/filelock.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <linux/rcupdate.h>
#include <linux/pid_namespace.h>
#include <linux/hashtable.h>
#include <linux/percpu.h>
#include <linux/sysctl.h>

#define CREATE_TRACE_POINTS
#include <trace/events/filelock.h>

#include <linux/uaccess.h>

static struct file_lock *file_lock(struct file_lock_core *flc)
{
	return container_of(flc, struct file_lock, c);
}

static struct file_lease *file_lease(struct file_lock_core *flc)
{
	return container_of(flc, struct file_lease, c);
}

static bool lease_breaking(struct file_lease *fl)
{
	return fl->c.flc_flags & (FL_UNLOCK_PENDING | FL_DOWNGRADE_PENDING);
}

static int target_leasetype(struct file_lease *fl)
{
	if (fl->c.flc_flags & FL_UNLOCK_PENDING)
		return F_UNLCK;
	if (fl->c.flc_flags & FL_DOWNGRADE_PENDING)
		return F_RDLCK;
	return fl->c.flc_type;
}

static int leases_enable = 1;
static int lease_break_time = 45;

#ifdef CONFIG_SYSCTL
static const struct ctl_table locks_sysctls[] = {
	{
		.procname	= "leases-enable",
		.data		= &leases_enable,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
#ifdef CONFIG_MMU
	{
		.procname	= "lease-break-time",
		.data		= &lease_break_time,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
#endif /* CONFIG_MMU */
};

static int __init init_fs_locks_sysctls(void)
{
	register_sysctl_init("fs", locks_sysctls);
	return 0;
}
early_initcall(init_fs_locks_sysctls);
#endif /* CONFIG_SYSCTL */

/*
 * The global file_lock_list is only used for displaying /proc/locks, so we
 * keep a list on each CPU, with each list protected by its own spinlock.
 * Global serialization is done using file_rwsem.
 *
 * Note that alterations to the list also require that the relevant flc_lock is
 * held.
 */
struct file_lock_list_struct {
	spinlock_t		lock;
	struct hlist_head	hlist;
};
static DEFINE_PER_CPU(struct file_lock_list_struct, file_lock_list);
DEFINE_STATIC_PERCPU_RWSEM(file_rwsem);


/*
 * The blocked_hash is used to find POSIX lock loops for deadlock detection.
 * It is protected by blocked_lock_lock.
 *
 * We hash locks by lockowner in order to optimize searching for the lock a
 * particular lockowner is waiting on.
 *
 * FIXME: make this value scale via some heuristic? We generally will want more
 * buckets when we have more lockowners holding locks, but that's a little
 * difficult to determine without knowing what the workload will look like.
 */
#define BLOCKED_HASH_BITS	7
static DEFINE_HASHTABLE(blocked_hash, BLOCKED_HASH_BITS);

/*
 * This lock protects the blocked_hash. Generally, if you're accessing it, you
 * want to be holding this lock.
 *
 * In addition, it also protects the fl->fl_blocked_requests list, and the
 * fl->fl_blocker pointer for file_lock structures that are acting as lock
 * requests (in contrast to those that are acting as records of acquired locks).
 *
 * Note that when we acquire this lock in order to change the above fields,
 * we often hold the flc_lock as well. In certain cases, when reading the fields
 * protected by this lock, we can skip acquiring it iff we already hold the
 * flc_lock.
 */
static DEFINE_SPINLOCK(blocked_lock_lock);

static struct kmem_cache *flctx_cache __ro_after_init;
static struct kmem_cache *filelock_cache __ro_after_init;
static struct kmem_cache *filelease_cache __ro_after_init;

static struct file_lock_context *
locks_get_lock_context(struct inode *inode, int type)
{
	struct file_lock_context *ctx;

	/* paired with cmpxchg() below */
	ctx = locks_inode_context(inode);
	if (likely(ctx) || type == F_UNLCK)
		goto out;

	ctx = kmem_cache_alloc(flctx_cache, GFP_KERNEL);
	if (!ctx)
		goto out;

	spin_lock_init(&ctx->flc_lock);
	INIT_LIST_HEAD(&ctx->flc_flock);
	INIT_LIST_HEAD(&ctx->flc_posix);
	INIT_LIST_HEAD(&ctx->flc_lease);

	/*
	 * Assign the pointer if it's not already assigned. If it is, then
	 * free the context we just allocated.
	 */
	if (cmpxchg(&inode->i_flctx, NULL, ctx)) {
		kmem_cache_free(flctx_cache, ctx);
		ctx = locks_inode_context(inode);
	}
out:
	trace_locks_get_lock_context(inode, type, ctx);
	return ctx;
}

static void
locks_dump_ctx_list(struct list_head *list, char *list_type)
{
	struct file_lock_core *flc;

	list_for_each_entry(flc, list, flc_list)
		pr_warn("%s: fl_owner=%p fl_flags=0x%x fl_type=0x%x fl_pid=%u\n",
			list_type, flc->flc_owner, flc->flc_flags,
			flc->flc_type, flc->flc_pid);
}

static void
locks_check_ctx_lists(struct inode *inode)
{
	struct file_lock_context *ctx = inode->i_flctx;

	if (unlikely(!list_empty(&ctx->flc_flock) ||
		     !list_empty(&ctx->flc_posix) ||
		     !list_empty(&ctx->flc_lease))) {
		pr_warn("Leaked locks on dev=0x%x:0x%x ino=0x%lx:\n",
			MAJOR(inode->i_sb->s_dev), MINOR(inode->i_sb->s_dev),
			inode->i_ino);
		locks_dump_ctx_list(&ctx->flc_flock, "FLOCK");
		locks_dump_ctx_list(&ctx->flc_posix, "POSIX");
		locks_dump_ctx_list(&ctx->flc_lease, "LEASE");
	}
}

static void
locks_check_ctx_file_list(struct file *filp, struct list_head *list, char *list_type)
{
	struct file_lock_core *flc;
	struct inode *inode = file_inode(filp);

	list_for_each_entry(flc, list, flc_list)
		if (flc->flc_file == filp)
			pr_warn("Leaked %s lock on dev=0x%x:0x%x ino=0x%lx "
				" fl_owner=%p fl_flags=0x%x fl_type=0x%x fl_pid=%u\n",
				list_type, MAJOR(inode->i_sb->s_dev),
				MINOR(inode->i_sb->s_dev), inode->i_ino,
				flc->flc_owner, flc->flc_flags,
				flc->flc_type, flc->flc_pid);
}

void
locks_free_lock_context(struct inode *inode)
{
	struct file_lock_context *ctx = locks_inode_context(inode);

	if (unlikely(ctx)) {
		locks_check_ctx_lists(inode);
		kmem_cache_free(flctx_cache, ctx);
	}
}

static void locks_init_lock_heads(struct file_lock_core *flc)
{
	INIT_HLIST_NODE(&flc->flc_link);
	INIT_LIST_HEAD(&flc->flc_list);
	INIT_LIST_HEAD(&flc->flc_blocked_requests);
	INIT_LIST_HEAD(&flc->flc_blocked_member);
	init_waitqueue_head(&flc->flc_wait);
}

/* Allocate an empty lock structure. */
struct file_lock *locks_alloc_lock(void)
{
	struct file_lock *fl = kmem_cache_zalloc(filelock_cache, GFP_KERNEL);

	if (fl)
		locks_init_lock_heads(&fl->c);

	return fl;
}
EXPORT_SYMBOL_GPL(locks_alloc_lock);

/* Allocate an empty lock structure. */
struct file_lease *locks_alloc_lease(void)
{
	struct file_lease *fl = kmem_cache_zalloc(filelease_cache, GFP_KERNEL);

	if (fl)
		locks_init_lock_heads(&fl->c);

	return fl;
}
EXPORT_SYMBOL_GPL(locks_alloc_lease);

void locks_release_private(struct file_lock *fl)
{
	struct file_lock_core *flc = &fl->c;

	BUG_ON(waitqueue_active(&flc->flc_wait));
	BUG_ON(!list_empty(&flc->flc_list));
	BUG_ON(!list_empty(&flc->flc_blocked_requests));
	BUG_ON(!list_empty(&flc->flc_blocked_member));
	BUG_ON(!hlist_unhashed(&flc->flc_link));

	if (fl->fl_ops) {
		if (fl->fl_ops->fl_release_private)
			fl->fl_ops->fl_release_private(fl);
		fl->fl_ops = NULL;
	}

	if (fl->fl_lmops) {
		if (fl->fl_lmops->lm_put_owner) {
			fl->fl_lmops->lm_put_owner(flc->flc_owner);
			flc->flc_owner = NULL;
		}
		fl->fl_lmops = NULL;
	}
}
EXPORT_SYMBOL_GPL(locks_release_private);

/**
 * locks_owner_has_blockers - Check for blocking lock requests
 * @flctx: file lock context
 * @owner: lock owner
 *
 * Return values:
 *   %true: @owner has at least one blocker
 *   %false: @owner has no blockers
 */
bool locks_owner_has_blockers(struct file_lock_context *flctx, fl_owner_t owner)
{
	struct file_lock_core *flc;

	spin_lock(&flctx->flc_lock);
	list_for_each_entry(flc, &flctx->flc_posix, flc_list) {
		if (flc->flc_owner != owner)
			continue;
		if (!list_empty(&flc->flc_blocked_requests)) {
			spin_unlock(&flctx->flc_lock);
			return true;
		}
	}
	spin_unlock(&flctx->flc_lock);
	return false;
}
EXPORT_SYMBOL_GPL(locks_owner_has_blockers);

/* Free a lock which is not in use. */
void locks_free_lock(struct file_lock *fl)
{
	locks_release_private(fl);
	kmem_cache_free(filelock_cache, fl);
}
EXPORT_SYMBOL(locks_free_lock);

/* Free a lease which is not in use. */
void locks_free_lease(struct file_lease *fl)
{
	kmem_cache_free(filelease_cache, fl);
}
EXPORT_SYMBOL(locks_free_lease);

static void
locks_dispose_list(struct list_head *dispose)
{
	struct file_lock_core *flc;

	while (!list_empty(dispose)) {
		flc = list_first_entry(dispose, struct file_lock_core, flc_list);
		list_del_init(&flc->flc_list);
		if (flc->flc_flags & (FL_LEASE|FL_DELEG|FL_LAYOUT))
			locks_free_lease(file_lease(flc));
		else
			locks_free_lock(file_lock(flc));
	}
}

void locks_init_lock(struct file_lock *fl)
{
	memset(fl, 0, sizeof(struct file_lock));
	locks_init_lock_heads(&fl->c);
}
EXPORT_SYMBOL(locks_init_lock);

void locks_init_lease(struct file_lease *fl)
{
	memset(fl, 0, sizeof(*fl));
	locks_init_lock_heads(&fl->c);
}
EXPORT_SYMBOL(locks_init_lease);

/*
 * Initialize a new lock from an existing file_lock structure.
 */
void locks_copy_conflock(struct file_lock *new, struct file_lock *fl)
{
	new->c.flc_owner = fl->c.flc_owner;
	new->c.flc_pid = fl->c.flc_pid;
	new->c.flc_file = NULL;
	new->c.flc_flags = fl->c.flc_flags;
	new->c.flc_type = fl->c.flc_type;
	new->fl_start = fl->fl_start;
	new->fl_end = fl->fl_end;
	new->fl_lmops = fl->fl_lmops;
	new->fl_ops = NULL;

	if (fl->fl_lmops) {
		if (fl->fl_lmops->lm_get_owner)
			fl->fl_lmops->lm_get_owner(fl->c.flc_owner);
	}
}
EXPORT_SYMBOL(locks_copy_conflock);

void locks_copy_lock(struct file_lock *new, struct file_lock *fl)
{
	/* "new" must be a freshly-initialized lock */
	WARN_ON_ONCE(new->fl_ops);

	locks_copy_conflock(new, fl);

	new->c.flc_file = fl->c.flc_file;
	new->fl_ops = fl->fl_ops;

	if (fl->fl_ops) {
		if (fl->fl_ops->fl_copy_lock)
			fl->fl_ops->fl_copy_lock(new, fl);
	}
}
EXPORT_SYMBOL(locks_copy_lock);

static void locks_move_blocks(struct file_lock *new, struct file_lock *fl)
{
	struct file_lock *f;

	/*
	 * As ctx->flc_lock is held, new requests cannot be added to
	 * ->flc_blocked_requests, so we don't need a lock to check if it
	 * is empty.
	 */
	if (list_empty(&fl->c.flc_blocked_requests))
		return;
	spin_lock(&blocked_lock_lock);
	list_splice_init(&fl->c.flc_blocked_requests,
			 &new->c.flc_blocked_requests);
	list_for_each_entry(f, &new->c.flc_blocked_requests,
			    c.flc_blocked_member)
		f->c.flc_blocker = &new->c;
	spin_unlock(&blocked_lock_lock);
}

static inline int flock_translate_cmd(int cmd) {
	switch (cmd) {
	case LOCK_SH:
		return F_RDLCK;
	case LOCK_EX:
		return F_WRLCK;
	case LOCK_UN:
		return F_UNLCK;
	}
	return -EINVAL;
}

/* Fill in a file_lock structure with an appropriate FLOCK lock. */
static void flock_make_lock(struct file *filp, struct file_lock *fl, int type)
{
	locks_init_lock(fl);

	fl->c.flc_file = filp;
	fl->c.flc_owner = filp;
	fl->c.flc_pid = current->tgid;
	fl->c.flc_flags = FL_FLOCK;
	fl->c.flc_type = type;
	fl->fl_end = OFFSET_MAX;
}

static int assign_type(struct file_lock_core *flc, int type)
{
	switch (type) {
	case F_RDLCK:
	case F_WRLCK:
	case F_UNLCK:
		flc->flc_type = type;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int flock64_to_posix_lock(struct file *filp, struct file_lock *fl,
				 struct flock64 *l)
{
	switch (l->l_whence) {
	case SEEK_SET:
		fl->fl_start = 0;
		break;
	case SEEK_CUR:
		fl->fl_start = filp->f_pos;
		break;
	case SEEK_END:
		fl->fl_start = i_size_read(file_inode(filp));
		break;
	default:
		return -EINVAL;
	}
	if (l->l_start > OFFSET_MAX - fl->fl_start)
		return -EOVERFLOW;
	fl->fl_start += l->l_start;
	if (fl->fl_start < 0)
		return -EINVAL;

	/* POSIX-1996 leaves the case l->l_len < 0 undefined;
	   POSIX-2001 defines it. */
	if (l->l_len > 0) {
		if (l->l_len - 1 > OFFSET_MAX - fl->fl_start)
			return -EOVERFLOW;
		fl->fl_end = fl->fl_start + (l->l_len - 1);

	} else if (l->l_len < 0) {
		if (fl->fl_start + l->l_len < 0)
			return -EINVAL;
		fl->fl_end = fl->fl_start - 1;
		fl->fl_start += l->l_len;
	} else
		fl->fl_end = OFFSET_MAX;

	fl->c.flc_owner = current->files;
	fl->c.flc_pid = current->tgid;
	fl->c.flc_file = filp;
	fl->c.flc_flags = FL_POSIX;
	fl->fl_ops = NULL;
	fl->fl_lmops = NULL;

	return assign_type(&fl->c, l->l_type);
}

/* Verify a "struct flock" and copy it to a "struct file_lock" as a POSIX
 * style lock.
 */
static int flock_to_posix_lock(struct file *filp, struct file_lock *fl,
			       struct flock *l)
{
	struct flock64 ll = {
		.l_type = l->l_type,
		.l_whence = l->l_whence,
		.l_start = l->l_start,
		.l_len = l->l_len,
	};

	return flock64_to_posix_lock(filp, fl, &ll);
}

/* default lease lock manager operations */
static bool
lease_break_callback(struct file_lease *fl)
{
	kill_fasync(&fl->fl_fasync, SIGIO, POLL_MSG);
	return false;
}

static void
lease_setup(struct file_lease *fl, void **priv)
{
	struct file *filp = fl->c.flc_file;
	struct fasync_struct *fa = *priv;

	/*
	 * fasync_insert_entry() returns the old entry if any. If there was no
	 * old entry, then it used "priv" and inserted it into the fasync list.
	 * Clear the pointer to indicate that it shouldn't be freed.
	 */
	if (!fasync_insert_entry(fa->fa_fd, filp, &fl->fl_fasync, fa))
		*priv = NULL;

	__f_setown(filp, task_pid(current), PIDTYPE_TGID, 0);
}

static const struct lease_manager_operations lease_manager_ops = {
	.lm_break = lease_break_callback,
	.lm_change = lease_modify,
	.lm_setup = lease_setup,
};

/*
 * Initialize a lease, use the default lock manager operations
 */
static int lease_init(struct file *filp, int type, struct file_lease *fl)
{
	if (assign_type(&fl->c, type) != 0)
		return -EINVAL;

	fl->c.flc_owner = filp;
	fl->c.flc_pid = current->tgid;

	fl->c.flc_file = filp;
	fl->c.flc_flags = FL_LEASE;
	fl->fl_lmops = &lease_manager_ops;
	return 0;
}

/* Allocate a file_lock initialised to this type of lease */
static struct file_lease *lease_alloc(struct file *filp, int type)
{
	struct file_lease *fl = locks_alloc_lease();
	int error = -ENOMEM;

	if (fl == NULL)
		return ERR_PTR(error);

	error = lease_init(filp, type, fl);
	if (error) {
		locks_free_lease(fl);
		return ERR_PTR(error);
	}
	return fl;
}

/* Check if two locks overlap each other.
 */
static inline int locks_overlap(struct file_lock *fl1, struct file_lock *fl2)
{
	return ((fl1->fl_end >= fl2->fl_start) &&
		(fl2->fl_end >= fl1->fl_start));
}

/*
 * Check whether two locks have the same owner.
 */
static int posix_same_owner(struct file_lock_core *fl1, struct file_lock_core *fl2)
{
	return fl1->flc_owner == fl2->flc_owner;
}

/* Must be called with the flc_lock held! */
static void locks_insert_global_locks(struct file_lock_core *flc)
{
	struct file_lock_list_struct *fll = this_cpu_ptr(&file_lock_list);

	percpu_rwsem_assert_held(&file_rwsem);

	spin_lock(&fll->lock);
	flc->flc_link_cpu = smp_processor_id();
	hlist_add_head(&flc->flc_link, &fll->hlist);
	spin_unlock(&fll->lock);
}

/* Must be called with the flc_lock held! */
static void locks_delete_global_locks(struct file_lock_core *flc)
{
	struct file_lock_list_struct *fll;

	percpu_rwsem_assert_held(&file_rwsem);

	/*
	 * Avoid taking lock if already unhashed. This is safe since this check
	 * is done while holding the flc_lock, and new insertions into the list
	 * also require that it be held.
	 */
	if (hlist_unhashed(&flc->flc_link))
		return;

	fll = per_cpu_ptr(&file_lock_list, flc->flc_link_cpu);
	spin_lock(&fll->lock);
	hlist_del_init(&flc->flc_link);
	spin_unlock(&fll->lock);
}

static unsigned long
posix_owner_key(struct file_lock_core *flc)
{
	return (unsigned long) flc->flc_owner;
}

static void locks_insert_global_blocked(struct file_lock_core *waiter)
{
	lockdep_assert_held(&blocked_lock_lock);

	hash_add(blocked_hash, &waiter->flc_link, posix_owner_key(waiter));
}

static void locks_delete_global_blocked(struct file_lock_core *waiter)
{
	lockdep_assert_held(&blocked_lock_lock);

	hash_del(&waiter->flc_link);
}

/* Remove waiter from blocker's block list.
 * When blocker ends up pointing to itself then the list is empty.
 *
 * Must be called with blocked_lock_lock held.
 */
static void __locks_unlink_block(struct file_lock_core *waiter)
{
	locks_delete_global_blocked(waiter);
	list_del_init(&waiter->flc_blocked_member);
}

static void __locks_wake_up_blocks(struct file_lock_core *blocker)
{
	while (!list_empty(&blocker->flc_blocked_requests)) {
		struct file_lock_core *waiter;
		struct file_lock *fl;

		waiter = list_first_entry(&blocker->flc_blocked_requests,
					  struct file_lock_core, flc_blocked_member);

		fl = file_lock(waiter);
		__locks_unlink_block(waiter);
		if ((waiter->flc_flags & (FL_POSIX | FL_FLOCK)) &&
		    fl->fl_lmops && fl->fl_lmops->lm_notify)
			fl->fl_lmops->lm_notify(fl);
		else
			locks_wake_up_waiter(waiter);

		/*
		 * The setting of flc_blocker to NULL marks the "done"
		 * point in deleting a block. Paired with acquire at the top
		 * of locks_delete_block().
		 */
		smp_store_release(&waiter->flc_blocker, NULL);
	}
}

static int __locks_delete_block(struct file_lock_core *waiter)
{
	int status = -ENOENT;

	/*
	 * If fl_blocker is NULL, it won't be set again as this thread "owns"
	 * the lock and is the only one that might try to claim the lock.
	 *
	 * We use acquire/release to manage fl_blocker so that we can
	 * optimize away taking the blocked_lock_lock in many cases.
	 *
	 * The smp_load_acquire guarantees two things:
	 *
	 * 1/ that fl_blocked_requests can be tested locklessly. If something
	 * was recently added to that list it must have been in a locked region
	 * *before* the locked region when fl_blocker was set to NULL.
	 *
	 * 2/ that no other thread is accessing 'waiter', so it is safe to free
	 * it.  __locks_wake_up_blocks is careful not to touch waiter after
	 * fl_blocker is released.
	 *
	 * If a lockless check of fl_blocker shows it to be NULL, we know that
	 * no new locks can be inserted into its fl_blocked_requests list, and
	 * can avoid doing anything further if the list is empty.
	 */
	if (!smp_load_acquire(&waiter->flc_blocker) &&
	    list_empty(&waiter->flc_blocked_requests))
		return status;

	spin_lock(&blocked_lock_lock);
	if (waiter->flc_blocker)
		status = 0;
	__locks_wake_up_blocks(waiter);
	__locks_unlink_block(waiter);

	/*
	 * The setting of fl_blocker to NULL marks the "done" point in deleting
	 * a block. Paired with acquire at the top of this function.
	 */
	smp_store_release(&waiter->flc_blocker, NULL);
	spin_unlock(&blocked_lock_lock);
	return status;
}

/**
 *	locks_delete_block - stop waiting for a file lock
 *	@waiter: the lock which was waiting
 *
 *	lockd/nfsd need to disconnect the lock while working on it.
 */
int locks_delete_block(struct file_lock *waiter)
{
	return __locks_delete_block(&waiter->c);
}
EXPORT_SYMBOL(locks_delete_block);

/* Insert waiter into blocker's block list.
 * We use a circular list so that processes can be easily woken up in
 * the order they blocked. The documentation doesn't require this but
 * it seems like the reasonable thing to do.
 *
 * Must be called with both the flc_lock and blocked_lock_lock held. The
 * fl_blocked_requests list itself is protected by the blocked_lock_lock,
 * but by ensuring that the flc_lock is also held on insertions we can avoid
 * taking the blocked_lock_lock in some cases when we see that the
 * fl_blocked_requests list is empty.
 *
 * Rather than just adding to the list, we check for conflicts with any existing
 * waiters, and add beneath any waiter that blocks the new waiter.
 * Thus wakeups don't happen until needed.
 */
static void __locks_insert_block(struct file_lock_core *blocker,
				 struct file_lock_core *waiter,
				 bool conflict(struct file_lock_core *,
					       struct file_lock_core *))
{
	struct file_lock_core *flc;

	BUG_ON(!list_empty(&waiter->flc_blocked_member));
new_blocker:
	list_for_each_entry(flc, &blocker->flc_blocked_requests, flc_blocked_member)
		if (conflict(flc, waiter)) {
			blocker =  flc;
			goto new_blocker;
		}
	waiter->flc_blocker = blocker;
	list_add_tail(&waiter->flc_blocked_member,
		      &blocker->flc_blocked_requests);

	if ((blocker->flc_flags & (FL_POSIX|FL_OFDLCK)) == FL_POSIX)
		locks_insert_global_blocked(waiter);

	/* The requests in waiter->flc_blocked are known to conflict with
	 * waiter, but might not conflict with blocker, or the requests
	 * and lock which block it.  So they all need to be woken.
	 */
	__locks_wake_up_blocks(waiter);
}

/* Must be called with flc_lock held. */
static void locks_insert_block(struct file_lock_core *blocker,
			       struct file_lock_core *waiter,
			       bool conflict(struct file_lock_core *,
					     struct file_lock_core *))
{
	spin_lock(&blocked_lock_lock);
	__locks_insert_block(blocker, waiter, conflict);
	spin_unlock(&blocked_lock_lock);
}

/*
 * Wake up processes blocked waiting for blocker.
 *
 * Must be called with the inode->flc_lock held!
 */
static void locks_wake_up_blocks(struct file_lock_core *blocker)
{
	/*
	 * Avoid taking global lock if list is empty. This is safe since new
	 * blocked requests are only added to the list under the flc_lock, and
	 * the flc_lock is always held here. Note that removal from the
	 * fl_blocked_requests list does not require the flc_lock, so we must
	 * recheck list_empty() after acquiring the blocked_lock_lock.
	 */
	if (list_empty(&blocker->flc_blocked_requests))
		return;

	spin_lock(&blocked_lock_lock);
	__locks_wake_up_blocks(blocker);
	spin_unlock(&blocked_lock_lock);
}

static void
locks_insert_lock_ctx(struct file_lock_core *fl, struct list_head *before)
{
	list_add_tail(&fl->flc_list, before);
	locks_insert_global_locks(fl);
}

static void
locks_unlink_lock_ctx(struct file_lock_core *fl)
{
	locks_delete_global_locks(fl);
	list_del_init(&fl->flc_list);
	locks_wake_up_blocks(fl);
}

static void
locks_delete_lock_ctx(struct file_lock_core *fl, struct list_head *dispose)
{
	locks_unlink_lock_ctx(fl);
	if (dispose)
		list_add(&fl->flc_list, dispose);
	else
		locks_free_lock(file_lock(fl));
}

/* Determine if lock sys_fl blocks lock caller_fl. Common functionality
 * checks for shared/exclusive status of overlapping locks.
 */
static bool locks_conflict(struct file_lock_core *caller_flc,
			   struct file_lock_core *sys_flc)
{
	if (sys_flc->flc_type == F_WRLCK)
		return true;
	if (caller_flc->flc_type == F_WRLCK)
		return true;
	return false;
}

/* Determine if lock sys_fl blocks lock caller_fl. POSIX specific
 * checking before calling the locks_conflict().
 */
static bool posix_locks_conflict(struct file_lock_core *caller_flc,
				 struct file_lock_core *sys_flc)
{
	struct file_lock *caller_fl = file_lock(caller_flc);
	struct file_lock *sys_fl = file_lock(sys_flc);

	/* POSIX locks owned by the same process do not conflict with
	 * each other.
	 */
	if (posix_same_owner(caller_flc, sys_flc))
		return false;

	/* Check whether they overlap */
	if (!locks_overlap(caller_fl, sys_fl))
		return false;

	return locks_conflict(caller_flc, sys_flc);
}

/* Determine if lock sys_fl blocks lock caller_fl. Used on xx_GETLK
 * path so checks for additional GETLK-specific things like F_UNLCK.
 */
static bool posix_test_locks_conflict(struct file_lock *caller_fl,
				      struct file_lock *sys_fl)
{
	struct file_lock_core *caller = &caller_fl->c;
	struct file_lock_core *sys = &sys_fl->c;

	/* F_UNLCK checks any locks on the same fd. */
	if (lock_is_unlock(caller_fl)) {
		if (!posix_same_owner(caller, sys))
			return false;
		return locks_overlap(caller_fl, sys_fl);
	}
	return posix_locks_conflict(caller, sys);
}

/* Determine if lock sys_fl blocks lock caller_fl. FLOCK specific
 * checking before calling the locks_conflict().
 */
static bool flock_locks_conflict(struct file_lock_core *caller_flc,
				 struct file_lock_core *sys_flc)
{
	/* FLOCK locks referring to the same filp do not conflict with
	 * each other.
	 */
	if (caller_flc->flc_file == sys_flc->flc_file)
		return false;

	return locks_conflict(caller_flc, sys_flc);
}

void
posix_test_lock(struct file *filp, struct file_lock *fl)
{
	struct file_lock *cfl;
	struct file_lock_context *ctx;
	struct inode *inode = file_inode(filp);
	void *owner;
	void (*func)(void);

	ctx = locks_inode_context(inode);
	if (!ctx || list_empty_careful(&ctx->flc_posix)) {
		fl->c.flc_type = F_UNLCK;
		return;
	}

retry:
	spin_lock(&ctx->flc_lock);
	list_for_each_entry(cfl, &ctx->flc_posix, c.flc_list) {
		if (!posix_test_locks_conflict(fl, cfl))
			continue;
		if (cfl->fl_lmops && cfl->fl_lmops->lm_lock_expirable
			&& (*cfl->fl_lmops->lm_lock_expirable)(cfl)) {
			owner = cfl->fl_lmops->lm_mod_owner;
			func = cfl->fl_lmops->lm_expire_lock;
			__module_get(owner);
			spin_unlock(&ctx->flc_lock);
			(*func)();
			module_put(owner);
			goto retry;
		}
		locks_copy_conflock(fl, cfl);
		goto out;
	}
	fl->c.flc_type = F_UNLCK;
out:
	spin_unlock(&ctx->flc_lock);
	return;
}
EXPORT_SYMBOL(posix_test_lock);

/*
 * Deadlock detection:
 *
 * We attempt to detect deadlocks that are due purely to posix file
 * locks.
 *
 * We assume that a task can be waiting for at most one lock at a time.
 * So for any acquired lock, the process holding that lock may be
 * waiting on at most one other lock.  That lock in turns may be held by
 * someone waiting for at most one other lock.  Given a requested lock
 * caller_fl which is about to wait for a conflicting lock block_fl, we
 * follow this chain of waiters to ensure we are not about to create a
 * cycle.
 *
 * Since we do this before we ever put a process to sleep on a lock, we
 * are ensured that there is never a cycle; that is what guarantees that
 * the while() loop in posix_locks_deadlock() eventually completes.
 *
 * Note: the above assumption may not be true when handling lock
 * requests from a broken NFS client. It may also fail in the presence
 * of tasks (such as posix threads) sharing the same open file table.
 * To handle those cases, we just bail out after a few iterations.
 *
 * For FL_OFDLCK locks, the owner is the filp, not the files_struct.
 * Because the owner is not even nominally tied to a thread of
 * execution, the deadlock detection below can't reasonably work well. Just
 * skip it for those.
 *
 * In principle, we could do a more limited deadlock detection on FL_OFDLCK
 * locks that just checks for the case where two tasks are attempting to
 * upgrade from read to write locks on the same inode.
 */

#define MAX_DEADLK_ITERATIONS 10

/* Find a lock that the owner of the given @blocker is blocking on. */
static struct file_lock_core *what_owner_is_waiting_for(struct file_lock_core *blocker)
{
	struct file_lock_core *flc;

	hash_for_each_possible(blocked_hash, flc, flc_link, posix_owner_key(blocker)) {
		if (posix_same_owner(flc, blocker)) {
			while (flc->flc_blocker)
				flc = flc->flc_blocker;
			return flc;
		}
	}
	return NULL;
}

/* Must be called with the blocked_lock_lock held! */
static bool posix_locks_deadlock(struct file_lock *caller_fl,
				 struct file_lock *block_fl)
{
	struct file_lock_core *caller = &caller_fl->c;
	struct file_lock_core *blocker = &block_fl->c;
	int i = 0;

	lockdep_assert_held(&blocked_lock_lock);

	/*
	 * This deadlock detector can't reasonably detect deadlocks with
	 * FL_OFDLCK locks, since they aren't owned by a process, per-se.
	 */
	if (caller->flc_flags & FL_OFDLCK)
		return false;

	while ((blocker = what_owner_is_waiting_for(blocker))) {
		if (i++ > MAX_DEADLK_ITERATIONS)
			return false;
		if (posix_same_owner(caller, blocker))
			return true;
	}
	return false;
}

/* Try to create a FLOCK lock on filp. We always insert new FLOCK locks
 * after any leases, but before any posix locks.
 *
 * Note that if called with an FL_EXISTS argument, the caller may determine
 * whether or not a lock was successfully freed by testing the return
 * value for -ENOENT.
 */
static int flock_lock_inode(struct inode *inode, struct file_lock *request)
{
	struct file_lock *new_fl = NULL;
	struct file_lock *fl;
	struct file_lock_context *ctx;
	int error = 0;
	bool found = false;
	LIST_HEAD(dispose);

	ctx = locks_get_lock_context(inode, request->c.flc_type);
	if (!ctx) {
		if (request->c.flc_type != F_UNLCK)
			return -ENOMEM;
		return (request->c.flc_flags & FL_EXISTS) ? -ENOENT : 0;
	}

	if (!(request->c.flc_flags & FL_ACCESS) && (request->c.flc_type != F_UNLCK)) {
		new_fl = locks_alloc_lock();
		if (!new_fl)
			return -ENOMEM;
	}

	percpu_down_read(&file_rwsem);
	spin_lock(&ctx->flc_lock);
	if (request->c.flc_flags & FL_ACCESS)
		goto find_conflict;

	list_for_each_entry(fl, &ctx->flc_flock, c.flc_list) {
		if (request->c.flc_file != fl->c.flc_file)
			continue;
		if (request->c.flc_type == fl->c.flc_type)
			goto out;
		found = true;
		locks_delete_lock_ctx(&fl->c, &dispose);
		break;
	}

	if (lock_is_unlock(request)) {
		if ((request->c.flc_flags & FL_EXISTS) && !found)
			error = -ENOENT;
		goto out;
	}

find_conflict:
	list_for_each_entry(fl, &ctx->flc_flock, c.flc_list) {
		if (!flock_locks_conflict(&request->c, &fl->c))
			continue;
		error = -EAGAIN;
		if (!(request->c.flc_flags & FL_SLEEP))
			goto out;
		error = FILE_LOCK_DEFERRED;
		locks_insert_block(&fl->c, &request->c, flock_locks_conflict);
		goto out;
	}
	if (request->c.flc_flags & FL_ACCESS)
		goto out;
	locks_copy_lock(new_fl, request);
	locks_move_blocks(new_fl, request);
	locks_insert_lock_ctx(&new_fl->c, &ctx->flc_flock);
	new_fl = NULL;
	error = 0;

out:
	spin_unlock(&ctx->flc_lock);
	percpu_up_read(&file_rwsem);
	if (new_fl)
		locks_free_lock(new_fl);
	locks_dispose_list(&dispose);
	trace_flock_lock_inode(inode, request, error);
	return error;
}

static int posix_lock_inode(struct inode *inode, struct file_lock *request,
			    struct file_lock *conflock)
{
	struct file_lock *fl, *tmp;
	struct file_lock *new_fl = NULL;
	struct file_lock *new_fl2 = NULL;
	struct file_lock *left = NULL;
	struct file_lock *right = NULL;
	struct file_lock_context *ctx;
	int error;
	bool added = false;
	LIST_HEAD(dispose);
	void *owner;
	void (*func)(void);

	ctx = locks_get_lock_context(inode, request->c.flc_type);
	if (!ctx)
		return lock_is_unlock(request) ? 0 : -ENOMEM;

	/*
	 * We may need two file_lock structures for this operation,
	 * so we get them in advance to avoid races.
	 *
	 * In some cases we can be sure, that no new locks will be needed
	 */
	if (!(request->c.flc_flags & FL_ACCESS) &&
	    (request->c.flc_type != F_UNLCK ||
	     request->fl_start != 0 || request->fl_end != OFFSET_MAX)) {
		new_fl = locks_alloc_lock();
		new_fl2 = locks_alloc_lock();
	}

retry:
	percpu_down_read(&file_rwsem);
	spin_lock(&ctx->flc_lock);
	/*
	 * New lock request. Walk all POSIX locks and look for conflicts. If
	 * there are any, either return error or put the request on the
	 * blocker's list of waiters and the global blocked_hash.
	 */
	if (request->c.flc_type != F_UNLCK) {
		list_for_each_entry(fl, &ctx->flc_posix, c.flc_list) {
			if (!posix_locks_conflict(&request->c, &fl->c))
				continue;
			if (fl->fl_lmops && fl->fl_lmops->lm_lock_expirable
				&& (*fl->fl_lmops->lm_lock_expirable)(fl)) {
				owner = fl->fl_lmops->lm_mod_owner;
				func = fl->fl_lmops->lm_expire_lock;
				__module_get(owner);
				spin_unlock(&ctx->flc_lock);
				percpu_up_read(&file_rwsem);
				(*func)();
				module_put(owner);
				goto retry;
			}
			if (conflock)
				locks_copy_conflock(conflock, fl);
			error = -EAGAIN;
			if (!(request->c.flc_flags & FL_SLEEP))
				goto out;
			/*
			 * Deadlock detection and insertion into the blocked
			 * locks list must be done while holding the same lock!
			 */
			error = -EDEADLK;
			spin_lock(&blocked_lock_lock);
			/*
			 * Ensure that we don't find any locks blocked on this
			 * request during deadlock detection.
			 */
			__locks_wake_up_blocks(&request->c);
			if (likely(!posix_locks_deadlock(request, fl))) {
				error = FILE_LOCK_DEFERRED;
				__locks_insert_block(&fl->c, &request->c,
						     posix_locks_conflict);
			}
			spin_unlock(&blocked_lock_lock);
			goto out;
		}
	}

	/* If we're just looking for a conflict, we're done. */
	error = 0;
	if (request->c.flc_flags & FL_ACCESS)
		goto out;

	/* Find the first old lock with the same owner as the new lock */
	list_for_each_entry(fl, &ctx->flc_posix, c.flc_list) {
		if (posix_same_owner(&request->c, &fl->c))
			break;
	}

	/* Process locks with this owner. */
	list_for_each_entry_safe_from(fl, tmp, &ctx->flc_posix, c.flc_list) {
		if (!posix_same_owner(&request->c, &fl->c))
			break;

		/* Detect adjacent or overlapping regions (if same lock type) */
		if (request->c.flc_type == fl->c.flc_type) {
			/* In all comparisons of start vs end, use
			 * "start - 1" rather than "end + 1". If end
			 * is OFFSET_MAX, end + 1 will become negative.
			 */
			if (fl->fl_end < request->fl_start - 1)
				continue;
			/* If the next lock in the list has entirely bigger
			 * addresses than the new one, insert the lock here.
			 */
			if (fl->fl_start - 1 > request->fl_end)
				break;

			/* If we come here, the new and old lock are of the
			 * same type and adjacent or overlapping. Make one
			 * lock yielding from the lower start address of both
			 * locks to the higher end address.
			 */
			if (fl->fl_start > request->fl_start)
				fl->fl_start = request->fl_start;
			else
				request->fl_start = fl->fl_start;
			if (fl->fl_end < request->fl_end)
				fl->fl_end = request->fl_end;
			else
				request->fl_end = fl->fl_end;
			if (added) {
				locks_delete_lock_ctx(&fl->c, &dispose);
				continue;
			}
			request = fl;
			added = true;
		} else {
			/* Processing for different lock types is a bit
			 * more complex.
			 */
			if (fl->fl_end < request->fl_start)
				continue;
			if (fl->fl_start > request->fl_end)
				break;
			if (lock_is_unlock(request))
				added = true;
			if (fl->fl_start < request->fl_start)
				left = fl;
			/* If the next lock in the list has a higher end
			 * address than the new one, insert the new one here.
			 */
			if (fl->fl_end > request->fl_end) {
				right = fl;
				break;
			}
			if (fl->fl_start >= request->fl_start) {
				/* The new lock completely replaces an old
				 * one (This may happen several times).
				 */
				if (added) {
					locks_delete_lock_ctx(&fl->c, &dispose);
					continue;
				}
				/*
				 * Replace the old lock with new_fl, and
				 * remove the old one. It's safe to do the
				 * insert here since we know that we won't be
				 * using new_fl later, and that the lock is
				 * just replacing an existing lock.
				 */
				error = -ENOLCK;
				if (!new_fl)
					goto out;
				locks_copy_lock(new_fl, request);
				locks_move_blocks(new_fl, request);
				request = new_fl;
				new_fl = NULL;
				locks_insert_lock_ctx(&request->c,
						      &fl->c.flc_list);
				locks_delete_lock_ctx(&fl->c, &dispose);
				added = true;
			}
		}
	}

	/*
	 * The above code only modifies existing locks in case of merging or
	 * replacing. If new lock(s) need to be inserted all modifications are
	 * done below this, so it's safe yet to bail out.
	 */
	error = -ENOLCK; /* "no luck" */
	if (right && left == right && !new_fl2)
		goto out;

	error = 0;
	if (!added) {
		if (lock_is_unlock(request)) {
			if (request->c.flc_flags & FL_EXISTS)
				error = -ENOENT;
			goto out;
		}

		if (!new_fl) {
			error = -ENOLCK;
			goto out;
		}
		locks_copy_lock(new_fl, request);
		locks_move_blocks(new_fl, request);
		locks_insert_lock_ctx(&new_fl->c, &fl->c.flc_list);
		fl = new_fl;
		new_fl = NULL;
	}
	if (right) {
		if (left == right) {
			/* The new lock breaks the old one in two pieces,
			 * so we have to use the second new lock.
			 */
			left = new_fl2;
			new_fl2 = NULL;
			locks_copy_lock(left, right);
			locks_insert_lock_ctx(&left->c, &fl->c.flc_list);
		}
		right->fl_start = request->fl_end + 1;
		locks_wake_up_blocks(&right->c);
	}
	if (left) {
		left->fl_end = request->fl_start - 1;
		locks_wake_up_blocks(&left->c);
	}
 out:
	trace_posix_lock_inode(inode, request, error);
	spin_unlock(&ctx->flc_lock);
	percpu_up_read(&file_rwsem);
	/*
	 * Free any unused locks.
	 */
	if (new_fl)
		locks_free_lock(new_fl);
	if (new_fl2)
		locks_free_lock(new_fl2);
	locks_dispose_list(&dispose);

	return error;
}

/**
 * posix_lock_file - Apply a POSIX-style lock to a file
 * @filp: The file to apply the lock to
 * @fl: The lock to be applied
 * @conflock: Place to return a copy of the conflicting lock, if found.
 *
 * Add a POSIX style lock to a file.
 * We merge adjacent & overlapping locks whenever possible.
 * POSIX locks are sorted by owner task, then by starting address
 *
 * Note that if called with an FL_EXISTS argument, the caller may determine
 * whether or not a lock was successfully freed by testing the return
 * value for -ENOENT.
 */
int posix_lock_file(struct file *filp, struct file_lock *fl,
			struct file_lock *conflock)
{
	return posix_lock_inode(file_inode(filp), fl, conflock);
}
EXPORT_SYMBOL(posix_lock_file);

/**
 * posix_lock_inode_wait - Apply a POSIX-style lock to a file
 * @inode: inode of file to which lock request should be applied
 * @fl: The lock to be applied
 *
 * Apply a POSIX style lock request to an inode.
 */
static int posix_lock_inode_wait(struct inode *inode, struct file_lock *fl)
{
	int error;
	might_sleep ();
	for (;;) {
		error = posix_lock_inode(inode, fl, NULL);
		if (error != FILE_LOCK_DEFERRED)
			break;
		error = wait_event_interruptible(fl->c.flc_wait,
						 list_empty(&fl->c.flc_blocked_member));
		if (error)
			break;
	}
	locks_delete_block(fl);
	return error;
}

static void lease_clear_pending(struct file_lease *fl, int arg)
{
	switch (arg) {
	case F_UNLCK:
		fl->c.flc_flags &= ~FL_UNLOCK_PENDING;
		fallthrough;
	case F_RDLCK:
		fl->c.flc_flags &= ~FL_DOWNGRADE_PENDING;
	}
}

/* We already had a lease on this file; just change its type */
int lease_modify(struct file_lease *fl, int arg, struct list_head *dispose)
{
	int error = assign_type(&fl->c, arg);

	if (error)
		return error;
	lease_clear_pending(fl, arg);
	locks_wake_up_blocks(&fl->c);
	if (arg == F_UNLCK) {
		struct file *filp = fl->c.flc_file;

		f_delown(filp);
		file_f_owner(filp)->signum = 0;
		fasync_helper(0, fl->c.flc_file, 0, &fl->fl_fasync);
		if (fl->fl_fasync != NULL) {
			printk(KERN_ERR "locks_delete_lock: fasync == %p\n", fl->fl_fasync);
			fl->fl_fasync = NULL;
		}
		locks_delete_lock_ctx(&fl->c, dispose);
	}
	return 0;
}
EXPORT_SYMBOL(lease_modify);

static bool past_time(unsigned long then)
{
	if (!then)
		/* 0 is a special value meaning "this never expires": */
		return false;
	return time_after(jiffies, then);
}

static void time_out_leases(struct inode *inode, struct list_head *dispose)
{
	struct file_lock_context *ctx = inode->i_flctx;
	struct file_lease *fl, *tmp;

	lockdep_assert_held(&ctx->flc_lock);

	list_for_each_entry_safe(fl, tmp, &ctx->flc_lease, c.flc_list) {
		trace_time_out_leases(inode, fl);
		if (past_time(fl->fl_downgrade_time))
			lease_modify(fl, F_RDLCK, dispose);
		if (past_time(fl->fl_break_time))
			lease_modify(fl, F_UNLCK, dispose);
	}
}

static bool leases_conflict(struct file_lock_core *lc, struct file_lock_core *bc)
{
	bool rc;
	struct file_lease *lease = file_lease(lc);
	struct file_lease *breaker = file_lease(bc);

	if (lease->fl_lmops->lm_breaker_owns_lease
			&& lease->fl_lmops->lm_breaker_owns_lease(lease))
		return false;
	if ((bc->flc_flags & FL_LAYOUT) != (lc->flc_flags & FL_LAYOUT)) {
		rc = false;
		goto trace;
	}
	if ((bc->flc_flags & FL_DELEG) && (lc->flc_flags & FL_LEASE)) {
		rc = false;
		goto trace;
	}

	rc = locks_conflict(bc, lc);
trace:
	trace_leases_conflict(rc, lease, breaker);
	return rc;
}

static bool
any_leases_conflict(struct inode *inode, struct file_lease *breaker)
{
	struct file_lock_context *ctx = inode->i_flctx;
	struct file_lock_core *flc;

	lockdep_assert_held(&ctx->flc_lock);

	list_for_each_entry(flc, &ctx->flc_lease, flc_list) {
		if (leases_conflict(flc, &breaker->c))
			return true;
	}
	return false;
}

/**
 *	__break_lease	-	revoke all outstanding leases on file
 *	@inode: the inode of the file to return
 *	@mode: O_RDONLY: break only write leases; O_WRONLY or O_RDWR:
 *	    break all leases
 *	@type: FL_LEASE: break leases and delegations; FL_DELEG: break
 *	    only delegations
 *
 *	break_lease (inlined for speed) has checked there already is at least
 *	some kind of lock (maybe a lease) on this file.  Leases are broken on
 *	a call to open() or truncate().  This function can sleep unless you
 *	specified %O_NONBLOCK to your open().
 */
int __break_lease(struct inode *inode, unsigned int mode, unsigned int type)
{
	int error = 0;
	struct file_lock_context *ctx;
	struct file_lease *new_fl, *fl, *tmp;
	unsigned long break_time;
	int want_write = (mode & O_ACCMODE) != O_RDONLY;
	LIST_HEAD(dispose);

	new_fl = lease_alloc(NULL, want_write ? F_WRLCK : F_RDLCK);
	if (IS_ERR(new_fl))
		return PTR_ERR(new_fl);
	new_fl->c.flc_flags = type;

	/* typically we will check that ctx is non-NULL before calling */
	ctx = locks_inode_context(inode);
	if (!ctx) {
		WARN_ON_ONCE(1);
		goto free_lock;
	}

	percpu_down_read(&file_rwsem);
	spin_lock(&ctx->flc_lock);

	time_out_leases(inode, &dispose);

	if (!any_leases_conflict(inode, new_fl))
		goto out;

	break_time = 0;
	if (lease_break_time > 0) {
		break_time = jiffies + lease_break_time * HZ;
		if (break_time == 0)
			break_time++;	/* so that 0 means no break time */
	}

	list_for_each_entry_safe(fl, tmp, &ctx->flc_lease, c.flc_list) {
		if (!leases_conflict(&fl->c, &new_fl->c))
			continue;
		if (want_write) {
			if (fl->c.flc_flags & FL_UNLOCK_PENDING)
				continue;
			fl->c.flc_flags |= FL_UNLOCK_PENDING;
			fl->fl_break_time = break_time;
		} else {
			if (lease_breaking(fl))
				continue;
			fl->c.flc_flags |= FL_DOWNGRADE_PENDING;
			fl->fl_downgrade_time = break_time;
		}
		if (fl->fl_lmops->lm_break(fl))
			locks_delete_lock_ctx(&fl->c, &dispose);
	}

	if (list_empty(&ctx->flc_lease))
		goto out;

	if (mode & O_NONBLOCK) {
		trace_break_lease_noblock(inode, new_fl);
		error = -EWOULDBLOCK;
		goto out;
	}

restart:
	fl = list_first_entry(&ctx->flc_lease, struct file_lease, c.flc_list);
	break_time = fl->fl_break_time;
	if (break_time != 0)
		break_time -= jiffies;
	if (break_time == 0)
		break_time++;
	locks_insert_block(&fl->c, &new_fl->c, leases_conflict);
	trace_break_lease_block(inode, new_fl);
	spin_unlock(&ctx->flc_lock);
	percpu_up_read(&file_rwsem);

	locks_dispose_list(&dispose);
	error = wait_event_interruptible_timeout(new_fl->c.flc_wait,
						 list_empty(&new_fl->c.flc_blocked_member),
						 break_time);

	percpu_down_read(&file_rwsem);
	spin_lock(&ctx->flc_lock);
	trace_break_lease_unblock(inode, new_fl);
	__locks_delete_block(&new_fl->c);
	if (error >= 0) {
		/*
		 * Wait for the next conflicting lease that has not been
		 * broken yet
		 */
		if (error == 0)
			time_out_leases(inode, &dispose);
		if (any_leases_conflict(inode, new_fl))
			goto restart;
		error = 0;
	}
out:
	spin_unlock(&ctx->flc_lock);
	percpu_up_read(&file_rwsem);
	locks_dispose_list(&dispose);
free_lock:
	locks_free_lease(new_fl);
	return error;
}
EXPORT_SYMBOL(__break_lease);

/**
 *	lease_get_mtime - update modified time of an inode with exclusive lease
 *	@inode: the inode
 *      @time:  pointer to a timespec which contains the last modified time
 *
 * This is to force NFS clients to flush their caches for files with
 * exclusive leases.  The justification is that if someone has an
 * exclusive lease, then they could be modifying it.
 */
void lease_get_mtime(struct inode *inode, struct timespec64 *time)
{
	bool has_lease = false;
	struct file_lock_context *ctx;
	struct file_lock_core *flc;

	ctx = locks_inode_context(inode);
	if (ctx && !list_empty_careful(&ctx->flc_lease)) {
		spin_lock(&ctx->flc_lock);
		flc = list_first_entry_or_null(&ctx->flc_lease,
					       struct file_lock_core, flc_list);
		if (flc && flc->flc_type == F_WRLCK)
			has_lease = true;
		spin_unlock(&ctx->flc_lock);
	}

	if (has_lease)
		*time = current_time(inode);
}
EXPORT_SYMBOL(lease_get_mtime);

/**
 *	fcntl_getlease - Enquire what lease is currently active
 *	@filp: the file
 *
 *	The value returned by this function will be one of
 *	(if no lease break is pending):
 *
 *	%F_RDLCK to indicate a shared lease is held.
 *
 *	%F_WRLCK to indicate an exclusive lease is held.
 *
 *	%F_UNLCK to indicate no lease is held.
 *
 *	(if a lease break is pending):
 *
 *	%F_RDLCK to indicate an exclusive lease needs to be
 *		changed to a shared lease (or removed).
 *
 *	%F_UNLCK to indicate the lease needs to be removed.
 *
 *	XXX: sfr & willy disagree over whether F_INPROGRESS
 *	should be returned to userspace.
 */
int fcntl_getlease(struct file *filp)
{
	struct file_lease *fl;
	struct inode *inode = file_inode(filp);
	struct file_lock_context *ctx;
	int type = F_UNLCK;
	LIST_HEAD(dispose);

	ctx = locks_inode_context(inode);
	if (ctx && !list_empty_careful(&ctx->flc_lease)) {
		percpu_down_read(&file_rwsem);
		spin_lock(&ctx->flc_lock);
		time_out_leases(inode, &dispose);
		list_for_each_entry(fl, &ctx->flc_lease, c.flc_list) {
			if (fl->c.flc_file != filp)
				continue;
			type = target_leasetype(fl);
			break;
		}
		spin_unlock(&ctx->flc_lock);
		percpu_up_read(&file_rwsem);

		locks_dispose_list(&dispose);
	}
	return type;
}

/**
 * check_conflicting_open - see if the given file points to an inode that has
 *			    an existing open that would conflict with the
 *			    desired lease.
 * @filp:	file to check
 * @arg:	type of lease that we're trying to acquire
 * @flags:	current lock flags
 *
 * Check to see if there's an existing open fd on this file that would
 * conflict with the lease we're trying to set.
 */
static int
check_conflicting_open(struct file *filp, const int arg, int flags)
{
	struct inode *inode = file_inode(filp);
	int self_wcount = 0, self_rcount = 0;

	if (flags & FL_LAYOUT)
		return 0;
	if (flags & FL_DELEG)
		/* We leave these checks to the caller */
		return 0;

	if (arg == F_RDLCK)
		return inode_is_open_for_write(inode) ? -EAGAIN : 0;
	else if (arg != F_WRLCK)
		return 0;

	/*
	 * Make sure that only read/write count is from lease requestor.
	 * Note that this will result in denying write leases when i_writecount
	 * is negative, which is what we want.  (We shouldn't grant write leases
	 * on files open for execution.)
	 */
	if (filp->f_mode & FMODE_WRITE)
		self_wcount = 1;
	else if (filp->f_mode & FMODE_READ)
		self_rcount = 1;

	if (atomic_read(&inode->i_writecount) != self_wcount ||
	    atomic_read(&inode->i_readcount) != self_rcount)
		return -EAGAIN;

	return 0;
}

static int
generic_add_lease(struct file *filp, int arg, struct file_lease **flp, void **priv)
{
	struct file_lease *fl, *my_fl = NULL, *lease;
	struct inode *inode = file_inode(filp);
	struct file_lock_context *ctx;
	bool is_deleg = (*flp)->c.flc_flags & FL_DELEG;
	int error;
	LIST_HEAD(dispose);

	lease = *flp;
	trace_generic_add_lease(inode, lease);

	error = file_f_owner_allocate(filp);
	if (error)
		return error;

	/* Note that arg is never F_UNLCK here */
	ctx = locks_get_lock_context(inode, arg);
	if (!ctx)
		return -ENOMEM;

	/*
	 * In the delegation case we need mutual exclusion with
	 * a number of operations that take the i_rwsem.  We trylock
	 * because delegations are an optional optimization, and if
	 * there's some chance of a conflict--we'd rather not
	 * bother, maybe that's a sign this just isn't a good file to
	 * hand out a delegation on.
	 */
	if (is_deleg && !inode_trylock(inode))
		return -EAGAIN;

	percpu_down_read(&file_rwsem);
	spin_lock(&ctx->flc_lock);
	time_out_leases(inode, &dispose);
	error = check_conflicting_open(filp, arg, lease->c.flc_flags);
	if (error)
		goto out;

	/*
	 * At this point, we know that if there is an exclusive
	 * lease on this file, then we hold it on this filp
	 * (otherwise our open of this file would have blocked).
	 * And if we are trying to acquire an exclusive lease,
	 * then the file is not open by anyone (including us)
	 * except for this filp.
	 */
	error = -EAGAIN;
	list_for_each_entry(fl, &ctx->flc_lease, c.flc_list) {
		if (fl->c.flc_file == filp &&
		    fl->c.flc_owner == lease->c.flc_owner) {
			my_fl = fl;
			continue;
		}

		/*
		 * No exclusive leases if someone else has a lease on
		 * this file:
		 */
		if (arg == F_WRLCK)
			goto out;
		/*
		 * Modifying our existing lease is OK, but no getting a
		 * new lease if someone else is opening for write:
		 */
		if (fl->c.flc_flags & FL_UNLOCK_PENDING)
			goto out;
	}

	if (my_fl != NULL) {
		lease = my_fl;
		error = lease->fl_lmops->lm_change(lease, arg, &dispose);
		if (error)
			goto out;
		goto out_setup;
	}

	error = -EINVAL;
	if (!leases_enable)
		goto out;

	locks_insert_lock_ctx(&lease->c, &ctx->flc_lease);
	/*
	 * The check in break_lease() is lockless. It's possible for another
	 * open to race in after we did the earlier check for a conflicting
	 * open but before the lease was inserted. Check again for a
	 * conflicting open and cancel the lease if there is one.
	 *
	 * We also add a barrier here to ensure that the insertion of the lock
	 * precedes these checks.
	 */
	smp_mb();
	error = check_conflicting_open(filp, arg, lease->c.flc_flags);
	if (error) {
		locks_unlink_lock_ctx(&lease->c);
		goto out;
	}

out_setup:
	if (lease->fl_lmops->lm_setup)
		lease->fl_lmops->lm_setup(lease, priv);
out:
	spin_unlock(&ctx->flc_lock);
	percpu_up_read(&file_rwsem);
	locks_dispose_list(&dispose);
	if (is_deleg)
		inode_unlock(inode);
	if (!error && !my_fl)
		*flp = NULL;
	return error;
}

static int generic_delete_lease(struct file *filp, void *owner)
{
	int error = -EAGAIN;
	struct file_lease *fl, *victim = NULL;
	struct inode *inode = file_inode(filp);
	struct file_lock_context *ctx;
	LIST_HEAD(dispose);

	ctx = locks_inode_context(inode);
	if (!ctx) {
		trace_generic_delete_lease(inode, NULL);
		return error;
	}

	percpu_down_read(&file_rwsem);
	spin_lock(&ctx->flc_lock);
	list_for_each_entry(fl, &ctx->flc_lease, c.flc_list) {
		if (fl->c.flc_file == filp &&
		    fl->c.flc_owner == owner) {
			victim = fl;
			break;
		}
	}
	trace_generic_delete_lease(inode, victim);
	if (victim)
		error = fl->fl_lmops->lm_change(victim, F_UNLCK, &dispose);
	spin_unlock(&ctx->flc_lock);
	percpu_up_read(&file_rwsem);
	locks_dispose_list(&dispose);
	return error;
}

/**
 *	generic_setlease	-	sets a lease on an open file
 *	@filp:	file pointer
 *	@arg:	type of lease to obtain
 *	@flp:	input - file_lock to use, output - file_lock inserted
 *	@priv:	private data for lm_setup (may be NULL if lm_setup
 *		doesn't require it)
 *
 *	The (input) flp->fl_lmops->lm_break function is required
 *	by break_lease().
 */
int generic_setlease(struct file *filp, int arg, struct file_lease **flp,
			void **priv)
{
	switch (arg) {
	case F_UNLCK:
		return generic_delete_lease(filp, *priv);
	case F_RDLCK:
	case F_WRLCK:
		if (!(*flp)->fl_lmops->lm_break) {
			WARN_ON_ONCE(1);
			return -ENOLCK;
		}

		return generic_add_lease(filp, arg, flp, priv);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL(generic_setlease);

/*
 * Kernel subsystems can register to be notified on any attempt to set
 * a new lease with the lease_notifier_chain. This is used by (e.g.) nfsd
 * to close files that it may have cached when there is an attempt to set a
 * conflicting lease.
 */
static struct srcu_notifier_head lease_notifier_chain;

static inline void
lease_notifier_chain_init(void)
{
	srcu_init_notifier_head(&lease_notifier_chain);
}

static inline void
setlease_notifier(int arg, struct file_lease *lease)
{
	if (arg != F_UNLCK)
		srcu_notifier_call_chain(&lease_notifier_chain, arg, lease);
}

int lease_register_notifier(struct notifier_block *nb)
{
	return srcu_notifier_chain_register(&lease_notifier_chain, nb);
}
EXPORT_SYMBOL_GPL(lease_register_notifier);

void lease_unregister_notifier(struct notifier_block *nb)
{
	srcu_notifier_chain_unregister(&lease_notifier_chain, nb);
}
EXPORT_SYMBOL_GPL(lease_unregister_notifier);


int
kernel_setlease(struct file *filp, int arg, struct file_lease **lease, void **priv)
{
	if (lease)
		setlease_notifier(arg, *lease);
	if (filp->f_op->setlease)
		return filp->f_op->setlease(filp, arg, lease, priv);
	else
		return generic_setlease(filp, arg, lease, priv);
}
EXPORT_SYMBOL_GPL(kernel_setlease);

/**
 * vfs_setlease        -       sets a lease on an open file
 * @filp:	file pointer
 * @arg:	type of lease to obtain
 * @lease:	file_lock to use when adding a lease
 * @priv:	private info for lm_setup when adding a lease (may be
 *		NULL if lm_setup doesn't require it)
 *
 * Call this to establish a lease on the file. The "lease" argument is not
 * used for F_UNLCK requests and may be NULL. For commands that set or alter
 * an existing lease, the ``(*lease)->fl_lmops->lm_break`` operation must be
 * set; if not, this function will return -ENOLCK (and generate a scary-looking
 * stack trace).
 *
 * The "priv" pointer is passed directly to the lm_setup function as-is. It
 * may be NULL if the lm_setup operation doesn't require it.
 */
int
vfs_setlease(struct file *filp, int arg, struct file_lease **lease, void **priv)
{
	struct inode *inode = file_inode(filp);
	vfsuid_t vfsuid = i_uid_into_vfsuid(file_mnt_idmap(filp), inode);
	int error;

	if ((!vfsuid_eq_kuid(vfsuid, current_fsuid())) && !capable(CAP_LEASE))
		return -EACCES;
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;
	error = security_file_lock(filp, arg);
	if (error)
		return error;
	return kernel_setlease(filp, arg, lease, priv);
}
EXPORT_SYMBOL_GPL(vfs_setlease);

static int do_fcntl_add_lease(unsigned int fd, struct file *filp, int arg)
{
	struct file_lease *fl;
	struct fasync_struct *new;
	int error;

	fl = lease_alloc(filp, arg);
	if (IS_ERR(fl))
		return PTR_ERR(fl);

	new = fasync_alloc();
	if (!new) {
		locks_free_lease(fl);
		return -ENOMEM;
	}
	new->fa_fd = fd;

	error = vfs_setlease(filp, arg, &fl, (void **)&new);
	if (fl)
		locks_free_lease(fl);
	if (new)
		fasync_free(new);
	return error;
}

/**
 *	fcntl_setlease	-	sets a lease on an open file
 *	@fd: open file descriptor
 *	@filp: file pointer
 *	@arg: type of lease to obtain
 *
 *	Call this fcntl to establish a lease on the file.
 *	Note that you also need to call %F_SETSIG to
 *	receive a signal when the lease is broken.
 */
int fcntl_setlease(unsigned int fd, struct file *filp, int arg)
{
	if (arg == F_UNLCK)
		return vfs_setlease(filp, F_UNLCK, NULL, (void **)&filp);
	return do_fcntl_add_lease(fd, filp, arg);
}

/**
 * flock_lock_inode_wait - Apply a FLOCK-style lock to a file
 * @inode: inode of the file to apply to
 * @fl: The lock to be applied
 *
 * Apply a FLOCK style lock request to an inode.
 */
static int flock_lock_inode_wait(struct inode *inode, struct file_lock *fl)
{
	int error;
	might_sleep();
	for (;;) {
		error = flock_lock_inode(inode, fl);
		if (error != FILE_LOCK_DEFERRED)
			break;
		error = wait_event_interruptible(fl->c.flc_wait,
						 list_empty(&fl->c.flc_blocked_member));
		if (error)
			break;
	}
	locks_delete_block(fl);
	return error;
}

/**
 * locks_lock_inode_wait - Apply a lock to an inode
 * @inode: inode of the file to apply to
 * @fl: The lock to be applied
 *
 * Apply a POSIX or FLOCK style lock request to an inode.
 */
int locks_lock_inode_wait(struct inode *inode, struct file_lock *fl)
{
	int res = 0;
	switch (fl->c.flc_flags & (FL_POSIX|FL_FLOCK)) {
		case FL_POSIX:
			res = posix_lock_inode_wait(inode, fl);
			break;
		case FL_FLOCK:
			res = flock_lock_inode_wait(inode, fl);
			break;
		default:
			BUG();
	}
	return res;
}
EXPORT_SYMBOL(locks_lock_inode_wait);

/**
 *	sys_flock: - flock() system call.
 *	@fd: the file descriptor to lock.
 *	@cmd: the type of lock to apply.
 *
 *	Apply a %FL_FLOCK style lock to an open file descriptor.
 *	The @cmd can be one of:
 *
 *	- %LOCK_SH -- a shared lock.
 *	- %LOCK_EX -- an exclusive lock.
 *	- %LOCK_UN -- remove an existing lock.
 *	- %LOCK_MAND -- a 'mandatory' flock. (DEPRECATED)
 *
 *	%LOCK_MAND support has been removed from the kernel.
 */
SYSCALL_DEFINE2(flock, unsigned int, fd, unsigned int, cmd)
{
	int can_sleep, error, type;
	struct file_lock fl;

	/*
	 * LOCK_MAND locks were broken for a long time in that they never
	 * conflicted with one another and didn't prevent any sort of open,
	 * read or write activity.
	 *
	 * Just ignore these requests now, to preserve legacy behavior, but
	 * throw a warning to let people know that they don't actually work.
	 */
	if (cmd & LOCK_MAND) {
		pr_warn_once("%s(%d): Attempt to set a LOCK_MAND lock via flock(2). This support has been removed and the request ignored.\n", current->comm, current->pid);
		return 0;
	}

	type = flock_translate_cmd(cmd & ~LOCK_NB);
	if (type < 0)
		return type;

	CLASS(fd, f)(fd);
	if (fd_empty(f))
		return -EBADF;

	if (type != F_UNLCK && !(fd_file(f)->f_mode & (FMODE_READ | FMODE_WRITE)))
		return -EBADF;

	flock_make_lock(fd_file(f), &fl, type);

	error = security_file_lock(fd_file(f), fl.c.flc_type);
	if (error)
		return error;

	can_sleep = !(cmd & LOCK_NB);
	if (can_sleep)
		fl.c.flc_flags |= FL_SLEEP;

	if (fd_file(f)->f_op->flock)
		error = fd_file(f)->f_op->flock(fd_file(f),
					    (can_sleep) ? F_SETLKW : F_SETLK,
					    &fl);
	else
		error = locks_lock_file_wait(fd_file(f), &fl);

	locks_release_private(&fl);
	return error;
}

/**
 * vfs_test_lock - test file byte range lock
 * @filp: The file to test lock for
 * @fl: The lock to test; also used to hold result
 *
 * Returns -ERRNO on failure.  Indicates presence of conflicting lock by
 * setting conf->fl_type to something other than F_UNLCK.
 */
int vfs_test_lock(struct file *filp, struct file_lock *fl)
{
	WARN_ON_ONCE(filp != fl->c.flc_file);
	if (filp->f_op->lock)
		return filp->f_op->lock(filp, F_GETLK, fl);
	posix_test_lock(filp, fl);
	return 0;
}
EXPORT_SYMBOL_GPL(vfs_test_lock);

/**
 * locks_translate_pid - translate a file_lock's fl_pid number into a namespace
 * @fl: The file_lock who's fl_pid should be translated
 * @ns: The namespace into which the pid should be translated
 *
 * Used to translate a fl_pid into a namespace virtual pid number
 */
static pid_t locks_translate_pid(struct file_lock_core *fl, struct pid_namespace *ns)
{
	pid_t vnr;
	struct pid *pid;

	if (fl->flc_flags & FL_OFDLCK)
		return -1;

	/* Remote locks report a negative pid value */
	if (fl->flc_pid <= 0)
		return fl->flc_pid;

	/*
	 * If the flock owner process is dead and its pid has been already
	 * freed, the translation below won't work, but we still want to show
	 * flock owner pid number in init pidns.
	 */
	if (ns == &init_pid_ns)
		return (pid_t) fl->flc_pid;

	rcu_read_lock();
	pid = find_pid_ns(fl->flc_pid, &init_pid_ns);
	vnr = pid_nr_ns(pid, ns);
	rcu_read_unlock();
	return vnr;
}

static int posix_lock_to_flock(struct flock *flock, struct file_lock *fl)
{
	flock->l_pid = locks_translate_pid(&fl->c, task_active_pid_ns(current));
#if BITS_PER_LONG == 32
	/*
	 * Make sure we can represent the posix lock via
	 * legacy 32bit flock.
	 */
	if (fl->fl_start > OFFT_OFFSET_MAX)
		return -EOVERFLOW;
	if (fl->fl_end != OFFSET_MAX && fl->fl_end > OFFT_OFFSET_MAX)
		return -EOVERFLOW;
#endif
	flock->l_start = fl->fl_start;
	flock->l_len = fl->fl_end == OFFSET_MAX ? 0 :
		fl->fl_end - fl->fl_start + 1;
	flock->l_whence = 0;
	flock->l_type = fl->c.flc_type;
	return 0;
}

#if BITS_PER_LONG == 32
static void posix_lock_to_flock64(struct flock64 *flock, struct file_lock *fl)
{
	flock->l_pid = locks_translate_pid(&fl->c, task_active_pid_ns(current));
	flock->l_start = fl->fl_start;
	flock->l_len = fl->fl_end == OFFSET_MAX ? 0 :
		fl->fl_end - fl->fl_start + 1;
	flock->l_whence = 0;
	flock->l_type = fl->c.flc_type;
}
#endif

/* Report the first existing lock that would conflict with l.
 * This implements the F_GETLK command of fcntl().
 */
int fcntl_getlk(struct file *filp, unsigned int cmd, struct flock *flock)
{
	struct file_lock *fl;
	int error;

	fl = locks_alloc_lock();
	if (fl == NULL)
		return -ENOMEM;
	error = -EINVAL;
	if (cmd != F_OFD_GETLK && flock->l_type != F_RDLCK
			&& flock->l_type != F_WRLCK)
		goto out;

	error = flock_to_posix_lock(filp, fl, flock);
	if (error)
		goto out;

	if (cmd == F_OFD_GETLK) {
		error = -EINVAL;
		if (flock->l_pid != 0)
			goto out;

		fl->c.flc_flags |= FL_OFDLCK;
		fl->c.flc_owner = filp;
	}

	error = vfs_test_lock(filp, fl);
	if (error)
		goto out;

	flock->l_type = fl->c.flc_type;
	if (fl->c.flc_type != F_UNLCK) {
		error = posix_lock_to_flock(flock, fl);
		if (error)
			goto out;
	}
out:
	locks_free_lock(fl);
	return error;
}

/**
 * vfs_lock_file - file byte range lock
 * @filp: The file to apply the lock to
 * @cmd: type of locking operation (F_SETLK, F_GETLK, etc.)
 * @fl: The lock to be applied
 * @conf: Place to return a copy of the conflicting lock, if found.
 *
 * A caller that doesn't care about the conflicting lock may pass NULL
 * as the final argument.
 *
 * If the filesystem defines a private ->lock() method, then @conf will
 * be left unchanged; so a caller that cares should initialize it to
 * some acceptable default.
 *
 * To avoid blocking kernel daemons, such as lockd, that need to acquire POSIX
 * locks, the ->lock() interface may return asynchronously, before the lock has
 * been granted or denied by the underlying filesystem, if (and only if)
 * lm_grant is set. Additionally EXPORT_OP_ASYNC_LOCK in export_operations
 * flags need to be set.
 *
 * Callers expecting ->lock() to return asynchronously will only use F_SETLK,
 * not F_SETLKW; they will set FL_SLEEP if (and only if) the request is for a
 * blocking lock. When ->lock() does return asynchronously, it must return
 * FILE_LOCK_DEFERRED, and call ->lm_grant() when the lock request completes.
 * If the request is for non-blocking lock the file system should return
 * FILE_LOCK_DEFERRED then try to get the lock and call the callback routine
 * with the result. If the request timed out the callback routine will return a
 * nonzero return code and the file system should release the lock. The file
 * system is also responsible to keep a corresponding posix lock when it
 * grants a lock so the VFS can find out which locks are locally held and do
 * the correct lock cleanup when required.
 * The underlying filesystem must not drop the kernel lock or call
 * ->lm_grant() before returning to the caller with a FILE_LOCK_DEFERRED
 * return code.
 */
int vfs_lock_file(struct file *filp, unsigned int cmd, struct file_lock *fl, struct file_lock *conf)
{
	WARN_ON_ONCE(filp != fl->c.flc_file);
	if (filp->f_op->lock)
		return filp->f_op->lock(filp, cmd, fl);
	else
		return posix_lock_file(filp, fl, conf);
}
EXPORT_SYMBOL_GPL(vfs_lock_file);

static int do_lock_file_wait(struct file *filp, unsigned int cmd,
			     struct file_lock *fl)
{
	int error;

	error = security_file_lock(filp, fl->c.flc_type);
	if (error)
		return error;

	for (;;) {
		error = vfs_lock_file(filp, cmd, fl, NULL);
		if (error != FILE_LOCK_DEFERRED)
			break;
		error = wait_event_interruptible(fl->c.flc_wait,
						 list_empty(&fl->c.flc_blocked_member));
		if (error)
			break;
	}
	locks_delete_block(fl);

	return error;
}

/* Ensure that fl->fl_file has compatible f_mode for F_SETLK calls */
static int
check_fmode_for_setlk(struct file_lock *fl)
{
	switch (fl->c.flc_type) {
	case F_RDLCK:
		if (!(fl->c.flc_file->f_mode & FMODE_READ))
			return -EBADF;
		break;
	case F_WRLCK:
		if (!(fl->c.flc_file->f_mode & FMODE_WRITE))
			return -EBADF;
	}
	return 0;
}

/* Apply the lock described by l to an open file descriptor.
 * This implements both the F_SETLK and F_SETLKW commands of fcntl().
 */
int fcntl_setlk(unsigned int fd, struct file *filp, unsigned int cmd,
		struct flock *flock)
{
	struct file_lock *file_lock = locks_alloc_lock();
	struct inode *inode = file_inode(filp);
	struct file *f;
	int error;

	if (file_lock == NULL)
		return -ENOLCK;

	error = flock_to_posix_lock(filp, file_lock, flock);
	if (error)
		goto out;

	error = check_fmode_for_setlk(file_lock);
	if (error)
		goto out;

	/*
	 * If the cmd is requesting file-private locks, then set the
	 * FL_OFDLCK flag and override the owner.
	 */
	switch (cmd) {
	case F_OFD_SETLK:
		error = -EINVAL;
		if (flock->l_pid != 0)
			goto out;

		cmd = F_SETLK;
		file_lock->c.flc_flags |= FL_OFDLCK;
		file_lock->c.flc_owner = filp;
		break;
	case F_OFD_SETLKW:
		error = -EINVAL;
		if (flock->l_pid != 0)
			goto out;

		cmd = F_SETLKW;
		file_lock->c.flc_flags |= FL_OFDLCK;
		file_lock->c.flc_owner = filp;
		fallthrough;
	case F_SETLKW:
		file_lock->c.flc_flags |= FL_SLEEP;
	}

	error = do_lock_file_wait(filp, cmd, file_lock);

	/*
	 * Detect close/fcntl races and recover by zapping all POSIX locks
	 * associated with this file and our files_struct, just like on
	 * filp_flush(). There is no need to do that when we're
	 * unlocking though, or for OFD locks.
	 */
	if (!error && file_lock->c.flc_type != F_UNLCK &&
	    !(file_lock->c.flc_flags & FL_OFDLCK)) {
		struct files_struct *files = current->files;
		/*
		 * We need that spin_lock here - it prevents reordering between
		 * update of i_flctx->flc_posix and check for it done in
		 * close(). rcu_read_lock() wouldn't do.
		 */
		spin_lock(&files->file_lock);
		f = files_lookup_fd_locked(files, fd);
		spin_unlock(&files->file_lock);
		if (f != filp) {
			locks_remove_posix(filp, files);
			error = -EBADF;
		}
	}
out:
	trace_fcntl_setlk(inode, file_lock, error);
	locks_free_lock(file_lock);
	return error;
}

#if BITS_PER_LONG == 32
/* Report the first existing lock that would conflict with l.
 * This implements the F_GETLK command of fcntl().
 */
int fcntl_getlk64(struct file *filp, unsigned int cmd, struct flock64 *flock)
{
	struct file_lock *fl;
	int error;

	fl = locks_alloc_lock();
	if (fl == NULL)
		return -ENOMEM;

	error = -EINVAL;
	if (cmd != F_OFD_GETLK && flock->l_type != F_RDLCK
			&& flock->l_type != F_WRLCK)
		goto out;

	error = flock64_to_posix_lock(filp, fl, flock);
	if (error)
		goto out;

	if (cmd == F_OFD_GETLK) {
		error = -EINVAL;
		if (flock->l_pid != 0)
			goto out;

		fl->c.flc_flags |= FL_OFDLCK;
		fl->c.flc_owner = filp;
	}

	error = vfs_test_lock(filp, fl);
	if (error)
		goto out;

	flock->l_type = fl->c.flc_type;
	if (fl->c.flc_type != F_UNLCK)
		posix_lock_to_flock64(flock, fl);

out:
	locks_free_lock(fl);
	return error;
}

/* Apply the lock described by l to an open file descriptor.
 * This implements both the F_SETLK and F_SETLKW commands of fcntl().
 */
int fcntl_setlk64(unsigned int fd, struct file *filp, unsigned int cmd,
		struct flock64 *flock)
{
	struct file_lock *file_lock = locks_alloc_lock();
	struct file *f;
	int error;

	if (file_lock == NULL)
		return -ENOLCK;

	error = flock64_to_posix_lock(filp, file_lock, flock);
	if (error)
		goto out;

	error = check_fmode_for_setlk(file_lock);
	if (error)
		goto out;

	/*
	 * If the cmd is requesting file-private locks, then set the
	 * FL_OFDLCK flag and override the owner.
	 */
	switch (cmd) {
	case F_OFD_SETLK:
		error = -EINVAL;
		if (flock->l_pid != 0)
			goto out;

		cmd = F_SETLK64;
		file_lock->c.flc_flags |= FL_OFDLCK;
		file_lock->c.flc_owner = filp;
		break;
	case F_OFD_SETLKW:
		error = -EINVAL;
		if (flock->l_pid != 0)
			goto out;

		cmd = F_SETLKW64;
		file_lock->c.flc_flags |= FL_OFDLCK;
		file_lock->c.flc_owner = filp;
		fallthrough;
	case F_SETLKW64:
		file_lock->c.flc_flags |= FL_SLEEP;
	}

	error = do_lock_file_wait(filp, cmd, file_lock);

	/*
	 * Detect close/fcntl races and recover by zapping all POSIX locks
	 * associated with this file and our files_struct, just like on
	 * filp_flush(). There is no need to do that when we're
	 * unlocking though, or for OFD locks.
	 */
	if (!error && file_lock->c.flc_type != F_UNLCK &&
	    !(file_lock->c.flc_flags & FL_OFDLCK)) {
		struct files_struct *files = current->files;
		/*
		 * We need that spin_lock here - it prevents reordering between
		 * update of i_flctx->flc_posix and check for it done in
		 * close(). rcu_read_lock() wouldn't do.
		 */
		spin_lock(&files->file_lock);
		f = files_lookup_fd_locked(files, fd);
		spin_unlock(&files->file_lock);
		if (f != filp) {
			locks_remove_posix(filp, files);
			error = -EBADF;
		}
	}
out:
	locks_free_lock(file_lock);
	return error;
}
#endif /* BITS_PER_LONG == 32 */

/*
 * This function is called when the file is being removed
 * from the task's fd array.  POSIX locks belonging to this task
 * are deleted at this time.
 */
void locks_remove_posix(struct file *filp, fl_owner_t owner)
{
	int error;
	struct inode *inode = file_inode(filp);
	struct file_lock lock;
	struct file_lock_context *ctx;

	/*
	 * If there are no locks held on this file, we don't need to call
	 * posix_lock_file().  Another process could be setting a lock on this
	 * file at the same time, but we wouldn't remove that lock anyway.
	 */
	ctx = locks_inode_context(inode);
	if (!ctx || list_empty(&ctx->flc_posix))
		return;

	locks_init_lock(&lock);
	lock.c.flc_type = F_UNLCK;
	lock.c.flc_flags = FL_POSIX | FL_CLOSE;
	lock.fl_start = 0;
	lock.fl_end = OFFSET_MAX;
	lock.c.flc_owner = owner;
	lock.c.flc_pid = current->tgid;
	lock.c.flc_file = filp;
	lock.fl_ops = NULL;
	lock.fl_lmops = NULL;

	error = vfs_lock_file(filp, F_SETLK, &lock, NULL);

	if (lock.fl_ops && lock.fl_ops->fl_release_private)
		lock.fl_ops->fl_release_private(&lock);
	trace_locks_remove_posix(inode, &lock, error);
}
EXPORT_SYMBOL(locks_remove_posix);

/* The i_flctx must be valid when calling into here */
static void
locks_remove_flock(struct file *filp, struct file_lock_context *flctx)
{
	struct file_lock fl;
	struct inode *inode = file_inode(filp);

	if (list_empty(&flctx->flc_flock))
		return;

	flock_make_lock(filp, &fl, F_UNLCK);
	fl.c.flc_flags |= FL_CLOSE;

	if (filp->f_op->flock)
		filp->f_op->flock(filp, F_SETLKW, &fl);
	else
		flock_lock_inode(inode, &fl);

	if (fl.fl_ops && fl.fl_ops->fl_release_private)
		fl.fl_ops->fl_release_private(&fl);
}

/* The i_flctx must be valid when calling into here */
static void
locks_remove_lease(struct file *filp, struct file_lock_context *ctx)
{
	struct file_lease *fl, *tmp;
	LIST_HEAD(dispose);

	if (list_empty(&ctx->flc_lease))
		return;

	percpu_down_read(&file_rwsem);
	spin_lock(&ctx->flc_lock);
	list_for_each_entry_safe(fl, tmp, &ctx->flc_lease, c.flc_list)
		if (filp == fl->c.flc_file)
			lease_modify(fl, F_UNLCK, &dispose);
	spin_unlock(&ctx->flc_lock);
	percpu_up_read(&file_rwsem);

	locks_dispose_list(&dispose);
}

/*
 * This function is called on the last close of an open file.
 */
void locks_remove_file(struct file *filp)
{
	struct file_lock_context *ctx;

	ctx = locks_inode_context(file_inode(filp));
	if (!ctx)
		return;

	/* remove any OFD locks */
	locks_remove_posix(filp, filp);

	/* remove flock locks */
	locks_remove_flock(filp, ctx);

	/* remove any leases */
	locks_remove_lease(filp, ctx);

	spin_lock(&ctx->flc_lock);
	locks_check_ctx_file_list(filp, &ctx->flc_posix, "POSIX");
	locks_check_ctx_file_list(filp, &ctx->flc_flock, "FLOCK");
	locks_check_ctx_file_list(filp, &ctx->flc_lease, "LEASE");
	spin_unlock(&ctx->flc_lock);
}

/**
 * vfs_cancel_lock - file byte range unblock lock
 * @filp: The file to apply the unblock to
 * @fl: The lock to be unblocked
 *
 * Used by lock managers to cancel blocked requests
 */
int vfs_cancel_lock(struct file *filp, struct file_lock *fl)
{
	WARN_ON_ONCE(filp != fl->c.flc_file);
	if (filp->f_op->lock)
		return filp->f_op->lock(filp, F_CANCELLK, fl);
	return 0;
}
EXPORT_SYMBOL_GPL(vfs_cancel_lock);

/**
 * vfs_inode_has_locks - are any file locks held on @inode?
 * @inode: inode to check for locks
 *
 * Return true if there are any FL_POSIX or FL_FLOCK locks currently
 * set on @inode.
 */
bool vfs_inode_has_locks(struct inode *inode)
{
	struct file_lock_context *ctx;
	bool ret;

	ctx = locks_inode_context(inode);
	if (!ctx)
		return false;

	spin_lock(&ctx->flc_lock);
	ret = !list_empty(&ctx->flc_posix) || !list_empty(&ctx->flc_flock);
	spin_unlock(&ctx->flc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vfs_inode_has_locks);

#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

struct locks_iterator {
	int	li_cpu;
	loff_t	li_pos;
};

static void lock_get_status(struct seq_file *f, struct file_lock_core *flc,
			    loff_t id, char *pfx, int repeat)
{
	struct inode *inode = NULL;
	unsigned int pid;
	struct pid_namespace *proc_pidns = proc_pid_ns(file_inode(f->file)->i_sb);
	int type = flc->flc_type;
	struct file_lock *fl = file_lock(flc);

	pid = locks_translate_pid(flc, proc_pidns);

	/*
	 * If lock owner is dead (and pid is freed) or not visible in current
	 * pidns, zero is shown as a pid value. Check lock info from
	 * init_pid_ns to get saved lock pid value.
	 */
	if (flc->flc_file != NULL)
		inode = file_inode(flc->flc_file);

	seq_printf(f, "%lld: ", id);

	if (repeat)
		seq_printf(f, "%*s", repeat - 1 + (int)strlen(pfx), pfx);

	if (flc->flc_flags & FL_POSIX) {
		if (flc->flc_flags & FL_ACCESS)
			seq_puts(f, "ACCESS");
		else if (flc->flc_flags & FL_OFDLCK)
			seq_puts(f, "OFDLCK");
		else
			seq_puts(f, "POSIX ");

		seq_printf(f, " %s ",
			     (inode == NULL) ? "*NOINODE*" : "ADVISORY ");
	} else if (flc->flc_flags & FL_FLOCK) {
		seq_puts(f, "FLOCK  ADVISORY  ");
	} else if (flc->flc_flags & (FL_LEASE|FL_DELEG|FL_LAYOUT)) {
		struct file_lease *lease = file_lease(flc);

		type = target_leasetype(lease);

		if (flc->flc_flags & FL_DELEG)
			seq_puts(f, "DELEG  ");
		else
			seq_puts(f, "LEASE  ");

		if (lease_breaking(lease))
			seq_puts(f, "BREAKING  ");
		else if (flc->flc_file)
			seq_puts(f, "ACTIVE    ");
		else
			seq_puts(f, "BREAKER   ");
	} else {
		seq_puts(f, "UNKNOWN UNKNOWN  ");
	}

	seq_printf(f, "%s ", (type == F_WRLCK) ? "WRITE" :
			     (type == F_RDLCK) ? "READ" : "UNLCK");
	if (inode) {
		/* userspace relies on this representation of dev_t */
		seq_printf(f, "%d %02x:%02x:%lu ", pid,
				MAJOR(inode->i_sb->s_dev),
				MINOR(inode->i_sb->s_dev), inode->i_ino);
	} else {
		seq_printf(f, "%d <none>:0 ", pid);
	}
	if (flc->flc_flags & FL_POSIX) {
		if (fl->fl_end == OFFSET_MAX)
			seq_printf(f, "%Ld EOF\n", fl->fl_start);
		else
			seq_printf(f, "%Ld %Ld\n", fl->fl_start, fl->fl_end);
	} else {
		seq_puts(f, "0 EOF\n");
	}
}

static struct file_lock_core *get_next_blocked_member(struct file_lock_core *node)
{
	struct file_lock_core *tmp;

	/* NULL node or root node */
	if (node == NULL || node->flc_blocker == NULL)
		return NULL;

	/* Next member in the linked list could be itself */
	tmp = list_next_entry(node, flc_blocked_member);
	if (list_entry_is_head(tmp, &node->flc_blocker->flc_blocked_requests,
			       flc_blocked_member)
		|| tmp == node) {
		return NULL;
	}

	return tmp;
}

static int locks_show(struct seq_file *f, void *v)
{
	struct locks_iterator *iter = f->private;
	struct file_lock_core *cur, *tmp;
	struct pid_namespace *proc_pidns = proc_pid_ns(file_inode(f->file)->i_sb);
	int level = 0;

	cur = hlist_entry(v, struct file_lock_core, flc_link);

	if (locks_translate_pid(cur, proc_pidns) == 0)
		return 0;

	/* View this crossed linked list as a binary tree, the first member of flc_blocked_requests
	 * is the left child of current node, the next silibing in flc_blocked_member is the
	 * right child, we can alse get the parent of current node from flc_blocker, so this
	 * question becomes traversal of a binary tree
	 */
	while (cur != NULL) {
		if (level)
			lock_get_status(f, cur, iter->li_pos, "-> ", level);
		else
			lock_get_status(f, cur, iter->li_pos, "", level);

		if (!list_empty(&cur->flc_blocked_requests)) {
			/* Turn left */
			cur = list_first_entry_or_null(&cur->flc_blocked_requests,
						       struct file_lock_core,
						       flc_blocked_member);
			level++;
		} else {
			/* Turn right */
			tmp = get_next_blocked_member(cur);
			/* Fall back to parent node */
			while (tmp == NULL && cur->flc_blocker != NULL) {
				cur = cur->flc_blocker;
				level--;
				tmp = get_next_blocked_member(cur);
			}
			cur = tmp;
		}
	}

	return 0;
}

static void __show_fd_locks(struct seq_file *f,
			struct list_head *head, int *id,
			struct file *filp, struct files_struct *files)
{
	struct file_lock_core *fl;

	list_for_each_entry(fl, head, flc_list) {

		if (filp != fl->flc_file)
			continue;
		if (fl->flc_owner != files && fl->flc_owner != filp)
			continue;

		(*id)++;
		seq_puts(f, "lock:\t");
		lock_get_status(f, fl, *id, "", 0);
	}
}

void show_fd_locks(struct seq_file *f,
		  struct file *filp, struct files_struct *files)
{
	struct inode *inode = file_inode(filp);
	struct file_lock_context *ctx;
	int id = 0;

	ctx = locks_inode_context(inode);
	if (!ctx)
		return;

	spin_lock(&ctx->flc_lock);
	__show_fd_locks(f, &ctx->flc_flock, &id, filp, files);
	__show_fd_locks(f, &ctx->flc_posix, &id, filp, files);
	__show_fd_locks(f, &ctx->flc_lease, &id, filp, files);
	spin_unlock(&ctx->flc_lock);
}

static void *locks_start(struct seq_file *f, loff_t *pos)
	__acquires(&blocked_lock_lock)
{
	struct locks_iterator *iter = f->private;

	iter->li_pos = *pos + 1;
	percpu_down_write(&file_rwsem);
	spin_lock(&blocked_lock_lock);
	return seq_hlist_start_percpu(&file_lock_list.hlist, &iter->li_cpu, *pos);
}

static void *locks_next(struct seq_file *f, void *v, loff_t *pos)
{
	struct locks_iterator *iter = f->private;

	++iter->li_pos;
	return seq_hlist_next_percpu(v, &file_lock_list.hlist, &iter->li_cpu, pos);
}

static void locks_stop(struct seq_file *f, void *v)
	__releases(&blocked_lock_lock)
{
	spin_unlock(&blocked_lock_lock);
	percpu_up_write(&file_rwsem);
}

static const struct seq_operations locks_seq_operations = {
	.start	= locks_start,
	.next	= locks_next,
	.stop	= locks_stop,
	.show	= locks_show,
};

static int __init proc_locks_init(void)
{
	proc_create_seq_private("locks", 0, NULL, &locks_seq_operations,
			sizeof(struct locks_iterator), NULL);
	return 0;
}
fs_initcall(proc_locks_init);
#endif

static int __init filelock_init(void)
{
	int i;

	flctx_cache = kmem_cache_create("file_lock_ctx",
			sizeof(struct file_lock_context), 0, SLAB_PANIC, NULL);

	filelock_cache = kmem_cache_create("file_lock_cache",
			sizeof(struct file_lock), 0, SLAB_PANIC, NULL);

	filelease_cache = kmem_cache_create("file_lease_cache",
			sizeof(struct file_lease), 0, SLAB_PANIC, NULL);

	for_each_possible_cpu(i) {
		struct file_lock_list_struct *fll = per_cpu_ptr(&file_lock_list, i);

		spin_lock_init(&fll->lock);
		INIT_HLIST_HEAD(&fll->hlist);
	}

	lease_notifier_chain_init();
	return 0;
}
core_initcall(filelock_init);

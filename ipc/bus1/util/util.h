// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_UTIL_UTIL_H
#define __B1_UTIL_UTIL_H

/**
 * DOC: Utilities
 *
 * XXX
 */

#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/uio.h>

/**
 * B1_TAIL - tail pointer in singly-linked lists
 *
 * Several places of bus1 use singly-linked lists. Usually, the tail pointer is
 * simply set to NULL. However, sometimes we need to be able to detect in O(1)
 * whether a node is linked. For that we set the tail pointer to B1_TAIL
 * rather than NULL.
 */
#define B1_TAIL ERR_PTR(-1)

/**
 * B1_WARN_ON() - warn if true
 * @_cond:		condition to test
 *
 * This macro is equivalent to `WARN_ON()`, but is a no-op if module testing is
 * disabled. We always evaluate side-effects!
 *
 * Return: The boolean result of @_cond is returned as int (just like WARN_ON).
 */
#ifdef CONFIG_BUS1_TEST
#  define B1_WARN_ON(_cond) WARN_ON(_cond)
#else
#  define B1_WARN_ON(_cond) ((int)!!(_cond))
#endif

/**
 * b1_assert_held() - lockdep assertion
 * @_lock:		lock to assert is held
 *
 * This macro is equivalent to `lockdep_assert_held()`, but is a no-op if
 * module testing is disabled. We always evaluate side-effects!
 */
#ifdef CONFIG_BUS1_TEST
#  define b1_assert_held(_lock) lockdep_assert_held(_lock)
#else
#  define b1_assert_held(_lock) ((void)(_lock))
#endif

int b1_import_vecs(struct iovec *vecsp,
		   size_t *n_vecsp,
		   const struct iovec __user *u_vecs,
		   size_t n_vecs);

/**
 * b1_lock2() - lock 2 mutexes
 * @a:			first mutex
 * @b:			second mutex
 *
 * This locks two mutexes at the same time. This assumes the locks have no
 * hierarchy assigned, and as such do not have a natural order to lock them in.
 * This function will order the locks based on their memory-address and then
 * lock them sequentially.
 *
 * Note that this is only safe if you never lock any mutexes of their
 * lock-class via other means at the same time.
 *
 * This function can safely be called with @a and @b pointing to the same lock,
 * in which case the lock is only taken once.
 */
static inline void b1_lock2(struct mutex *a, struct mutex *b)
{
	if (a < b) {
		mutex_lock(a);
		mutex_lock_nested(b, 1);
	} else if (a > b) {
		mutex_lock(b);
		mutex_lock_nested(a, 1);
	} else /* if (a == b) */ {
		mutex_lock(a);
	}
}

/**
 * b1_unlock2() - unlock 2 mutexes
 * @a:			first mutex
 * @b:			second mutex
 *
 * This unlocks two mutexes. See b1_lock2() for the counterpart.
 */
static inline void b1_unlock2(struct mutex *a, struct mutex *b)
{
	if (b != a)
		mutex_unlock(b);
	mutex_unlock(a);
}

#endif /* __B1_UTIL_UTIL_H */

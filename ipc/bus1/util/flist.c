// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include "flist.h"

/**
 * b1_flist_populate() - populate an flist
 * @list:		flist to operate on
 * @n:			number of elements
 * @gfp:		GFP to use for allocations
 *
 * Populate an flist. This pre-allocates the backing memory for an flist that
 * was statically initialized via b1_flist_init(). This is NOT needed if the
 * list was allocated via b1_flist_new().
 *
 * Return: 0 on success, negative error code on failure.
 */
int b1_flist_populate(struct b1_flist *list, size_t n, gfp_t gfp)
{
	if (gfp & __GFP_ZERO)
		memset(list, 0, b1_flist_inline_size(n));

	if (unlikely(n > B1_FLIST_BATCH)) {
		/*
		 * Never populate twice! We cannot verify the length of the
		 * fixed-list did not change, so lets just prevent this
		 * alltogether.
		 */
		WARN_ON(list[B1_FLIST_BATCH].next);

		n -= B1_FLIST_BATCH;
		list[B1_FLIST_BATCH].next = b1_flist_new(n, gfp);
		if (!list[B1_FLIST_BATCH].next)
			return -ENOMEM;
	}

	return 0;
}

/**
 * b1_flist_new() - allocate new flist
 * @n:			number of elements
 * @gfp:		GFP to use for allocations
 *
 * This allocates a new flist ready to store @n elements.
 *
 * Return: Pointer to flist, NULL if out-of-memory.
 */
struct b1_flist *b1_flist_new(size_t n, gfp_t gfp)
{
	struct b1_flist list, *e, *slot;
	size_t remaining;

	/*
	 * Static assertions via BUILD_BUG_ON() need function-context. We put
	 * them collectively here.
	 * We guarantee as API that flists can be accessed as pointer-arrays,
	 * so make sure that is actually true. Furthermore, verify we do not
	 * overallocate batches, but properly page-align them for best memory
	 * usage.
	 */
	BUILD_BUG_ON(sizeof(struct b1_flist) != sizeof(void *));
	BUILD_BUG_ON(sizeof(struct b1_flist) * (B1_FLIST_BATCH + 1) !=
		     PAGE_SIZE);

	list.next = NULL;
	slot = &list;
	remaining = n;

	while (remaining >= B1_FLIST_BATCH) {
		e = kmalloc_array(sizeof(*e), B1_FLIST_BATCH + 1, gfp);
		if (!e)
			return b1_flist_free(list.next, n);

		slot->next = e;
		slot = &e[B1_FLIST_BATCH];
		slot->next = NULL;

		remaining -= B1_FLIST_BATCH;
	}

	if (remaining > 0) {
		slot->next = kmalloc_array(remaining, sizeof(*e), gfp);
		if (!slot->next)
			return b1_flist_free(list.next, n);
	}

	return list.next;
}

/**
 * b1_flist_free() - free flist
 * @list:		flist to operate on, or NULL
 * @n:			number of elements
 *
 * This deallocates an flist previously created via b1_flist_new(). It is
 * safe to call this on partially populated flists.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct b1_flist *b1_flist_free(struct b1_flist *list, size_t n)
{
	struct b1_flist *e;

	if (list) {
		/*
		 * If @list was only partially allocated, then "next" pointers
		 * might be NULL. So check @list on each iteration.
		 */
		while (list && n >= B1_FLIST_BATCH) {
			e = list;
			list = list[B1_FLIST_BATCH].next;
			kfree(e);
			n -= B1_FLIST_BATCH;
		}

		kfree(list);
	}

	return NULL;
}

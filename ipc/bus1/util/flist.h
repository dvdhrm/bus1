// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_UTIL_FLIST_H
#define __B1_UTIL_FLIST_H

/**
 * DOC: Fixed Lists
 *
 * This implements a fixed-size list called b1_flist. The size of the list
 * must be constant over the lifetime of the list. The list can hold one
 * arbitrary pointer per node.
 *
 * Fixed lists are a combination of a linked list and a static array. That is,
 * fixed lists behave like linked lists (no random access, but arbitrary size),
 * but compare in speed with arrays (consecutive accesses are fast). Unlike
 * fixed arrays, fixed lists can hold huge number of elements without requiring
 * vmalloc(), but solely relying on small-size kmalloc() allocations.
 *
 * Internally, fixed lists are a singly-linked list of static arrays. This
 * guarantees that iterations behave almost like on an array, except when
 * crossing a batch-border.
 *
 * Fixed lists can replace fixed-size arrays whenever you need to support large
 * number of elements, but don't need random access. Fixed lists have ALMOST
 * the same memory requirements as fixed-size arrays, except one pointer of
 * state per 'B1_FLIST_BATCH' elements. If only a small size (i.e., it only
 * requires one batch) is stored in a fixed list, then its memory requirements
 * and iteration time are equivalent to fixed-size arrays.
 */

#include <linux/kernel.h>
#include <linux/mm.h>

/**
 * B1_FLIST_BATCH - Number of entries in a single batch
 *
 * The fixed-list implementation stores a given number of entries together in a
 * batch. B1_FLIST_BATCH defines the number of entries in these batches. Note
 * that fixed-lists never overallocate, so high batch-sizes don't waste space.
 * However, high batch-sizes require contiguous space, which might not be
 * available. Hence, we optimize them to always use exactly one page.
 *
 * A single batch stores one pointer per entry, plus one trailing pointer for
 * maintenance. Hence, our batch size will end up using exactly one page per
 * batch.
 */
#define B1_FLIST_BATCH (PAGE_SIZE / sizeof(void*) - 1)

/**
 * struct b1_flist - fixed list
 * @next:		pointer to next batch
 * @ptr:		stored entry
 */
struct b1_flist {
	union {
		struct b1_flist *next;
		void *ptr;
	};
};

int b1_flist_populate(struct b1_flist *flist, size_t n, gfp_t gfp);
struct b1_flist *b1_flist_new(size_t n, gfp_t gfp);
struct b1_flist *b1_flist_free(struct b1_flist *list, size_t n);

/**
 * b1_flist_inline_size() - calculate required inline size
 * @n:			number of entries
 *
 * When allocating storage for an flist, this calculates the size of the
 * initial array in bytes. Use b1_flist_new() directly if you want to
 * allocate an flist on the heap. This helper is only needed if you embed an
 * flist into another struct like this:
 *
 *     struct foo {
 *             ...
 *             struct b1_flist list[];
 *     };
 *
 * In that case the flist must be the last element, and the size in bytes
 * required by it is returned by this function.
 *
 * The inline-size of an flist is always bound to a fixed maximum. That is,
 * regardless of @n, this will always return a reasonable number that can be
 * allocated via kmalloc().
 *
 * Return: Size in bytes required for the initial batch of an flist.
 */
static inline size_t b1_flist_inline_size(size_t n)
{
	return sizeof(struct b1_flist) *
		((likely(n < B1_FLIST_BATCH)) ? n : (B1_FLIST_BATCH + 1));
}

/**
 * b1_flist_init() - initialize an flist
 * @list:		flist to initialize
 * @n:			number of entries
 *
 * This initializes an flist of size @n. It does NOT preallocate the memory,
 * but only initializes @list in a way that b1_flist_deinit() can be called
 * on it. Use b1_flist_populate() to populate the flist.
 *
 * This is only needed if your backing memory of @list is shared with another
 * object. If possible, use b1_flist_new() to allocate an flist on the heap
 * and avoid this dance.
 */
static inline void b1_flist_init(struct b1_flist *list, size_t n)
{
	if (unlikely(n >= B1_FLIST_BATCH))
		list[B1_FLIST_BATCH].next = NULL;
}

/**
 * b1_flist_deinit() - deinitialize an flist
 * @list:		flist to deinitialize
 * @n:			number of entries
 *
 * This deallocates an flist and releases all resources. If already
 * deinitialized, this is a no-op. This is only needed if you called
 * b1_flist_populate().
 */
static inline void b1_flist_deinit(struct b1_flist *list, size_t n)
{
	if (unlikely(n >= B1_FLIST_BATCH)) {
		b1_flist_free(list[B1_FLIST_BATCH].next, n - B1_FLIST_BATCH);
		list[B1_FLIST_BATCH].next = NULL;
	}
}

/**
 * b1_flist_next() - flist iterator
 * @iter:		iterator
 * @pos:		current position
 *
 * This advances an flist iterator by one position. @iter must point to the
 * current position, and the new position is returned by this function. @pos
 * must point to a variable that contains the current index position. That is,
 * @pos must be initialized to 0 and @iter to the flist head.
 *
 * Neither @pos nor @iter must be modified by anyone but this helper. In the
 * loop body you can use @iter->ptr to access the current element.
 *
 * This iterator is normally used like this:
 *
 *     size_t pos, n = 128;
 *     struct b1_flist *e, *list = b1_flist_new(n);
 *
 *     ...
 *
 *     for (pos = 0, e = list; pos < n; e = b1_flist_next(e, &pos)) {
 *             ... access e->ptr ...
 *     }
 *
 * Return: Next iterator position.
 */
static inline struct b1_flist *b1_flist_next(struct b1_flist *iter, size_t *pos)
{
	return (++*pos % B1_FLIST_BATCH) ? (iter + 1) : (iter + 1)->next;
}

/**
 * b1_flist_walk() - walk flist in batches
 * @list:		list to walk
 * @n:			number of entries
 * @iter:		iterator
 * @pos:		current position
 *
 * This walks an flist in batches of size up to B1_FLIST_BATCH. It is
 * normally used like this:
 *
 *     size_t pos, z, n = 65536;
 *     struct b1_flist *e, *list = b1_flist_new(n);
 *
 *     ...
 *
 *     pos = 0;
 *     while ((z = b1_flist_walk(list, n, &e, &pos)) > 0) {
 *             ... access e[0...z]->ptr
 *             ... invariant: z <= B1_FLIST_BATCH
 *             ... invariant: e[i]->ptr == (&e->ptr)[i]
 *     }
 *
 * Return: Size of batch at @iter.
 */
static inline size_t b1_flist_walk(struct b1_flist *list,
				   size_t n,
				   struct b1_flist **iter,
				   size_t *pos)
{
	if (*pos < n) {
		n = n - *pos;
		if (unlikely(n > B1_FLIST_BATCH))
			n = B1_FLIST_BATCH;
		if (likely(*pos == 0))
			*iter = list;
		else
			*iter = (*iter)[B1_FLIST_BATCH].next;
		*pos += n;
	} else {
		n = 0;
	}
	return n;
}

#endif /* __B1_UTIL_FLIST_H */

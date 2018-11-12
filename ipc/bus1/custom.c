// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include "core.h"
#include "custom.h"
#include "util/flist.h"
#include "util/util.h"

/**
 * b1_custom_node_new() - XXX
 */
struct b1_custom_node *b1_custom_node_new(struct b1_custom_shared *shared)
{
	struct b1_custom_node *node;

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return ERR_PTR(-ENOMEM);

	node->shared = b1_custom_shared_ref(shared);
	node->n_handles = 0;
	node->handles = NULL;

	return node;
}

/**
 * b1_custom_node_free() - XXX
 */
struct b1_custom_node *b1_custom_node_free(struct b1_custom_node *node)
{
	struct b1_custom_shared *shared;
	struct b1_flist *e;
	size_t pos;

	if (!node)
		return NULL;

	for (pos = 0, e = node->handles;
	     pos < node->n_handles;
	     e = b1_flist_next(e, &pos))
		e->ptr = b1_handle_unref(e->ptr);

	node->handles = b1_flist_free(node->handles, node->n_handles);

	shared = node->shared;
	if (node != &shared->inline_node)
		kfree(node);
	b1_custom_shared_unref(shared);

	return NULL;
}

static size_t b1_custom_shared_required_pages(size_t n_bytes)
{
	/*
	 * Calculate how many pages are needed to store @n_bytes bytes in a
	 * `struct b1_custom_shared` object. Since we store data in a plain
	 * page-array, it is as simple as dividing by the system page size.
	 */
	return DIV_ROUND_UP(n_bytes, PAGE_SIZE);
}

/**
 * b1_custom_shared_new() - XXX
 * @n_bytes:			number of bytes the object can hold
 *
 * Return: A reference to the new object is returned, ERR_PTR on failure.
 */
struct b1_custom_shared *b1_custom_shared_new(size_t n_bytes)
{
	struct b1_custom_shared *shared;
	struct b1_flist *e;
	size_t i, pos, n_pages;
	int r;

	n_pages = b1_custom_shared_required_pages(n_bytes);

	shared = kmalloc(sizeof(*shared), GFP_KERNEL);
	if (unlikely(!shared))
		return ERR_PTR(-ENOMEM);

	kref_init(&shared->ref);
	shared->n_bytes = n_bytes;

	r = 0;
	if (n_bytes <= sizeof(shared->inline_data)) {
		/* nothing to do */
	} else if (likely(n_pages <= ARRAY_SIZE(shared->inline_pages))) {
		for (i = 0; i < n_pages; ++i) {
			shared->inline_pages[i] = alloc_page(GFP_HIGHUSER |
							     __GFP_ACCOUNT);
			if (unlikely(!shared->inline_pages[i]))
				r = -ENOMEM;
		}
	} else {
		shared->list_pages = b1_flist_new(n_pages, GFP_KERNEL);
		if (unlikely(!shared->list_pages)) {
			r = -ENOMEM;
		} else {
			for (pos = 0, e = shared->list_pages;
			     pos < n_pages;
			     e = b1_flist_next(e, &pos)) {
				e->ptr = alloc_page(GFP_HIGHUSER |
						    __GFP_ACCOUNT);
				if (unlikely(!e->ptr))
					r = -ENOMEM;
			}
		}
	}
	if (unlikely(r < 0))
		goto error;

	return shared;

error:
	b1_custom_shared_unref(shared);
	return ERR_PTR(r);
}

/* internal callback for b1_custom_shared_unref() */
void b1_custom_shared_free_internal(struct kref *ref)
{
	struct b1_custom_shared *shared =
		container_of(ref, struct b1_custom_shared, ref);
	struct b1_flist *e;
	size_t i, pos, n_pages;

	n_pages = b1_custom_shared_required_pages(shared->n_bytes);

	if (shared->n_bytes <= sizeof(shared->inline_data)) {
		/* nothing to do */
	} else if (likely(n_pages <= ARRAY_SIZE(shared->inline_pages))) {
		for (i = 0; i < n_pages; ++i)
			put_page(shared->inline_pages[i]);
	} else if (shared->list_pages) {
		for (pos = 0, e = shared->list_pages;
		     pos < n_pages;
		     e = b1_flist_next(e, &pos))
			put_page(e->ptr);
		shared->list_pages = b1_flist_free(shared->list_pages, n_pages);
	}

	kfree(shared);
}

/**
 * b1_custom_shared_import() - XXX
 */
int b1_custom_shared_import(struct b1_custom_shared *shared,
			    struct iov_iter *iter)
{
	struct b1_flist *e;
	size_t i, n, pos, n_pages, n_total;

	n_total = iov_iter_count(iter);
	n_pages = b1_custom_shared_required_pages(shared->n_bytes);

	if (WARN_ON(n_total != shared->n_bytes))
		return -ENOTRECOVERABLE;

	if (shared->n_bytes <= sizeof(shared->inline_data)) {
		if (unlikely(!copy_from_iter_full(shared->inline_data,
						  shared->n_bytes,
						  iter)))
			return -EFAULT;
	} else if (likely(n_pages <= ARRAY_SIZE(shared->inline_pages))) {
		for (i = 0; i < n_pages; ++i) {
			n = copy_page_from_iter(shared->inline_pages[i],
						0, PAGE_SIZE, iter);
			if (unlikely(n < PAGE_SIZE && iov_iter_count(iter)))
				return -EFAULT;
		}
	} else {
		for (pos = 0, e = shared->list_pages;
		     pos < n_pages;
		     e = b1_flist_next(e, &pos)) {
			n = copy_page_from_iter(e->ptr, 0, PAGE_SIZE, iter);
			if (unlikely(n < PAGE_SIZE && iov_iter_count(iter)))
				return -EFAULT;
		}
	}

	return 0;
}

void b1_custom_stage_init(struct b1_custom_stage *stage)
{
	*stage = (struct b1_custom_stage){};
}

void b1_custom_stage_deinit(struct b1_custom_stage *stage)
{
	struct b1_flist *e;
	size_t i, pos;

	if (likely(stage->max_handles <= ARRAY_SIZE(stage->inline_handles))) {
		for (i = 0; i < stage->n_handles; ++i)
			b1_handle_unref(stage->inline_handles[i]);
	} else if (stage->list_handles) {
		for (pos = 0, e = stage->list_handles;
		     pos < stage->n_handles;
		     e = b1_flist_next(e, &pos))
			b1_handle_unref(e->ptr);
		stage->list_handles = b1_flist_free(stage->list_handles,
						    stage->max_handles);
	}

	stage->max_handles = 0;
	stage->n_handles = 0;
	stage->shared = b1_custom_shared_unref(stage->shared);
}

int b1_custom_stage_import(struct b1_custom_stage *stage,
			   size_t n_handles,
			   size_t n_data,
			   size_t n_data_vecs,
			   const struct iovec __user *u_data_vecs)
{
	struct iovec stack_vecs[UIO_FASTIOV];
	struct iovec *vecs = stack_vecs;
	struct iov_iter iter;
	size_t n_total;
	int r;

	if (WARN_ON(stage->shared))
		return -ENOTRECOVERABLE;

	if (unlikely(n_handles > ARRAY_SIZE(stage->inline_handles))) {
		stage->list_handles = b1_flist_new(n_handles, GFP_KERNEL);
		if (unlikely(!stage->list_handles)) {
			r = -ENOMEM;
			goto error;
		}
	}
	stage->max_handles = n_handles;

	stage->shared = b1_custom_shared_new(n_data);
	if (IS_ERR(stage->shared)) {
		r = PTR_ERR(stage->shared);
		stage->shared = NULL;
		goto error;
	}

	if (unlikely(n_data_vecs > ARRAY_SIZE(stack_vecs))) {
		vecs = kmalloc_array(n_data_vecs, sizeof(*vecs), GFP_KERNEL);
		if (unlikely(!vecs)) {
			r = -ENOMEM;
			goto error;
		}
	}

	r = b1_import_vecs(vecs, &n_total, u_data_vecs, n_data_vecs);
	if (unlikely(r < 0))
		goto error;

	iov_iter_init(&iter, WRITE, vecs, n_data_vecs, n_total);
	r = b1_custom_shared_import(stage->shared, &iter);
	if (unlikely(r < 0))
		goto error;

	if (unlikely(vecs != stack_vecs))
		kfree(vecs);
	vecs = NULL;

	return 0;

error:
	if (vecs != stack_vecs)
		kfree(vecs);
	b1_custom_stage_deinit(stage);
	return r;
}

// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/aio.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include "pool.h"

/**
 * b1_pool_slice_init() - initialize pool slice
 * @slice:		slice to operate on
 *
 * This initializes a pool slice and prepares it to be used for pool
 * allocations. If the slice is no longer needed, you must deinitialize it via
 * the b1_pool_slice_deinit() helper.
 */
void b1_pool_slice_init(struct b1_pool_slice *slice)
{
	*slice = (struct b1_pool_slice)B1_POOL_SLICE_NULL(*slice);
	RB_CLEAR_NODE(&slice->rb_offset);
	RB_CLEAR_NODE(&slice->rb_trailing);
}

/**
 * b1_pool_slice_deinit() - deinitialize pool slice
 * @slice:		slice to operate on
 *
 * This deinitializes a pool-slice. It is the responsibility of the caller to
 * deallocate the slice from a pool if it was allocated.
 */
void b1_pool_slice_deinit(struct b1_pool_slice *slice)
{
	WARN_ON(slice->pool);
	WARN_ON(!RB_EMPTY_NODE(&slice->rb_offset) ||
		!RB_EMPTY_NODE(&slice->rb_trailing));
}

/**
 * b1_pool_slice_write_iovec() - copy user memory to a slice
 * @slice:		slice to write to
 * @offset:		relative offset into slice memory
 * @iov:		iovec array, pointing to data to copy
 * @n_iov:		number of elements in @iov
 * @total_len:		total number of bytes to copy
 *
 * This copies the memory pointed to by @iov into the memory slice @slice at
 * relative offset @offset (relative to start of slice).
 *
 * This function always copies the entire request. If only a partial request
 * can be served, this function will fail.
 *
 * Return: Number of bytes copied, negative error code on failure.
 */
ssize_t b1_pool_slice_write_iovec(struct b1_pool_slice *slice,
				  loff_t offset,
				  struct iovec *iov,
				  size_t n_iov,
				  size_t total_len)
{
	struct iov_iter iter;
	ssize_t len;

	if (WARN_ON(!slice->pool))
		return -ENODEV;
	if (WARN_ON(offset + total_len < offset) ||
	    WARN_ON(offset + total_len > slice->size))
		return -EFAULT;
	if (total_len < 1)
		return 0;

	offset += slice->offset;
	iov_iter_init(&iter, WRITE, iov, n_iov, total_len);

	len = vfs_iter_write(slice->pool->shmem_file, &iter, &offset, 0);
	if (len >= 0 && len != total_len)
		len = -EFAULT;

	return len;
}

/**
 * b1_pool_slice_write_kvec() - copy kernel memory to a slice
 * @slice:		slice to write to
 * @offset:		relative offset into slice memory
 * @iov:		kvec array, pointing to data to copy
 * @n_iov:		number of elements in @iov
 * @total_len:		total number of bytes to copy
 *
 * This copies the memory pointed to by @iov into the memory slice @slice at
 * relative offset @offset (relative to begin of slice).
 *
 * This function always copies the entire request. If only a partial request
 * can be served, this function will fail.
 *
 * Return: Number of bytes copied, negative error code on failure.
 */
ssize_t b1_pool_slice_write_kvec(struct b1_pool_slice *slice,
				 loff_t offset,
				 struct kvec *iov,
				 size_t n_iov,
				 size_t total_len)
{
	struct iov_iter iter;
	mm_segment_t old_fs;
	ssize_t len;

	if (WARN_ON(!slice->pool))
		return -ENODEV;
	if (WARN_ON(offset + total_len < offset) ||
	    WARN_ON(offset + total_len > slice->size))
		return -EFAULT;
	if (total_len < 1)
		return 0;

	offset += slice->offset;
	iov_iter_kvec(&iter, WRITE | ITER_KVEC, iov, n_iov, total_len);

	old_fs = get_fs();
	set_fs(get_ds());
	len = vfs_iter_write(slice->pool->shmem_file, &iter, &offset, 0);
	if (len >= 0 && len != total_len)
		len = -EFAULT;
	set_fs(old_fs);

	return len;
}

static void b1_pool_slice_link_by_offset(struct b1_pool_slice *slice)
{
	struct b1_pool_slice *ps;
	struct rb_node **slot, *parent;

	WARN_ON(!slice->pool);
	WARN_ON(!RB_EMPTY_NODE(&slice->rb_offset));

	slot = &slice->pool->slices_by_offset.rb_node;
	parent = NULL;
	while (*slot) {
		ps = container_of(*slot, struct b1_pool_slice, rb_offset);
		parent = *slot;
		if (WARN_ON(slice->offset == ps->offset)) /* warn on dups */
			slot = &ps->rb_offset.rb_right;
		else if (slice->offset < ps->offset)
			slot = &ps->rb_offset.rb_left;
		else /* if (slice->offset > ps->offset) */
			slot = &ps->rb_offset.rb_right;
	}

	rb_link_node(&slice->rb_offset, parent, slot);
	rb_insert_color(&slice->rb_offset, &slice->pool->slices_by_offset);
}

static void b1_pool_slice_link_by_trailing(struct b1_pool_slice *slice)
{
	struct b1_pool_slice *ps;
	struct rb_node **slot, *parent;

	WARN_ON(!slice->pool || !slice->trailing);
	WARN_ON(!RB_EMPTY_NODE(&slice->rb_trailing));

	slot = &slice->pool->slices_by_trailing.rb_node;
	parent = NULL;
	while (*slot) {
		ps = container_of(*slot, struct b1_pool_slice, rb_trailing);
		parent = *slot;
		if (slice->trailing < ps->trailing)
			slot = &ps->rb_trailing.rb_left;
		else /* if (slice->trailing >= ps->trailing) */
			slot = &ps->rb_trailing.rb_right;
	}

	rb_link_node(&slice->rb_trailing, parent, slot);
	rb_insert_color(&slice->rb_trailing, &slice->pool->slices_by_trailing);
}

static void b1_pool_slice_link(struct b1_pool_slice *slice,
			       struct b1_pool *pool,
			       size_t offset,
			       size_t size,
			       size_t trailing)
{
	WARN_ON(!slice->pool);
	WARN_ON(!RB_EMPTY_NODE(&slice->rb_offset));
	WARN_ON(!RB_EMPTY_NODE(&slice->rb_trailing));

	slice->pool = pool;
	slice->offset = offset;
	slice->size = size;
	slice->trailing = trailing;

	if (slice->size > 0)
		b1_pool_slice_link_by_offset(slice);
	if (slice->trailing > 0)
		b1_pool_slice_link_by_trailing(slice);
}

/**
 * b1_pool_init() - create memory pool
 * @pool:		pool to operate on
 * @filename:		name to use for the shmem-file (only visible via /proc)
 *
 * Initialize a new pool object.
 *
 * Return: 0 on success, negative error code on failure.
 */
int b1_pool_init(struct b1_pool *pool, const char *filename)
{
	struct file *f;
	int r;

	/*
	 * A set of build-time assertions which need function-context and thus
	 * we placed them here in the root-level constructor.
	 * We verify that the maximum sizes fit into a u32, since we use
	 * u32-types for size+offset+etc. state-tracking.
	 */
	BUILD_BUG_ON(B1_POOL_SLICE_SIZE_MAX > U32_MAX);
	BUILD_BUG_ON(B1_POOL_SIZE > U32_MAX);

	/*
	 * We use a shmem-file as backing store of the allocation-pool. We
	 * treat it as virtually unlimited in size and rely on the caller to
	 * perform accounting before every allocation.
	 * We don't reserve the overcommit-area, so we pass VM_NORESERVE. The
	 * required pages are reserved on each individual request.
	 */
	f = shmem_file_setup(filename, PAGE_ALIGN(B1_POOL_SIZE), VM_NORESERVE);
	if (IS_ERR(f))
		return PTR_ERR(f);

	r = get_write_access(file_inode(f));
	if (r < 0) {
		fput(f);
		return r;
	}

	*pool = (struct b1_pool)B1_POOL_NULL(*pool);
	pool->shmem_file = f;
	b1_pool_slice_init(&pool->root_slice);

	/*
	 * We link the root slice with offset+size 0 and the entire pool as
	 * trailing space. All further allocations will chop off suitable
	 * chunks from this slice to serve their allocations.
	 */
	b1_pool_slice_link(&pool->root_slice, pool, 0, 0, B1_POOL_SIZE);

	return 0;
}

/**
 * b1_pool_deinit() - destroy pool
 * @pool:		pool to operate on
 *
 * This destroys a pool that was previously created via b1_pool_init(). The
 * caller must flush the pool before calling this. The must not be any
 * outstanding allocations on the pool!
 *
 * It is safe to call this multiple times on the same pool, or on a pool
 * initialized to B1_POOL_NULL.
 */
void b1_pool_deinit(struct b1_pool *pool)
{
	WARN_ON(!RB_EMPTY_ROOT(&pool->slices_by_offset));

	if (pool->shmem_file) {
		put_write_access(file_inode(pool->shmem_file));
		fput(pool->shmem_file);
		pool->shmem_file = NULL;
	}
}

/**
 * b1_pool_mmap() - mmap the pool
 * @pool:		pool to operate on
 * @vma:		VMA to map to
 *
 * This maps the backing shmem-file of the pool to the provided VMA.
 *
 * Return: 0 on success, negative error code on failure.
 */
int b1_pool_mmap(struct b1_pool *pool, struct vm_area_struct *vma)
{
	/* replace the connection file with our shmem file */
	if (vma->vm_file)
		fput(vma->vm_file);
	vma->vm_file = get_file(pool->shmem_file);

	/* calls into shmem_mmap(), which simply sets vm_ops */
	return pool->shmem_file->f_op->mmap(pool->shmem_file, vma);
}

/**
 * b1_pool_find_by_offset() - find slice by offset
 * @pool:		pool to operate on
 * @offset:		offset to get slice at
 *
 * Find the slice at the given offset, if it exists. This will only return a
 * slice if @offset refers to the start of the slice. An index somewhere into
 * the middle of a slice will not yield a positive result.
 *
 * Return: Slice at given offset is returned, or NULL if not found.
 */
struct b1_pool_slice *b1_pool_find_by_offset(struct b1_pool *pool, size_t o)
{
	struct b1_pool_slice *ps;
	struct rb_node *n;

	n = pool->slices_by_offset.rb_node;
	while (n) {
		ps = container_of(n, struct b1_pool_slice, rb_offset);
		if (o < ps->offset)
			n = n->rb_left;
		else if (o > ps->offset)
			n = n->rb_right;
		else /* if (o == ps->offset) */
			return ps;
	}

	return NULL;
}

static struct b1_pool_slice *b1_pool_find_by_trailing(struct b1_pool *pool,
						      size_t trailing)
{
	struct b1_pool_slice *ps, *closest = NULL;
	struct rb_node *n;

	n = pool->slices_by_trailing.rb_node;
	while (n) {
		ps = container_of(n, struct b1_pool_slice, rb_trailing);
		if (trailing < ps->trailing) {
			closest = ps;
			n = n->rb_left;
		} else if (trailing > ps->trailing) {
			n = n->rb_right;
		} else /* if (trailing == ps->trailing) */ {
			return ps;
		}
	}

	return closest;
}

/**
 * b1_pool_alloc() - allocate memory
 * @pool:		pool to operate on
 * @slice:		slice to allocate
 * @size:		number of bytes to allocate
 *
 * This allocates a new slice of @size bytes from the memory pool at @pool. The
 * slice must be released via b1_pool_dealloc() by the caller.
 *
 * The caller is responsible to manage the backing memory of the slice itself.
 * That is, @slice must be pre-allocated by the caller and released after
 * deallocating the slice. Furthermore, @slice must be initialized via
 * b1_pool_slice_init() before calling into this allocator.
 *
 * Slice allocations are aligned to 8-byte boundaries.
 *
 * Return: 0 on success, negative error code on failure.
 */
int b1_pool_alloc(struct b1_pool *pool,
		  struct b1_pool_slice *slice,
		  size_t size)
{
	struct b1_pool_slice *ps;
	size_t slice_size;

	if (WARN_ON(slice->pool))
		return -EALREADY;

	slice_size = ALIGN(size, 8);
	if (slice_size == 0 || slice_size > B1_POOL_SLICE_SIZE_MAX)
		return -E2BIG;

	ps = b1_pool_find_by_trailing(pool, slice_size);
	if (!ps)
		return -EXFULL;

	b1_pool_slice_link(slice,
			   pool,
			   ps->offset + ps->size,
			   slice_size,
			   ps->trailing - slice_size);

	rb_erase(&ps->rb_trailing, &pool->slices_by_trailing);
	ps->trailing = 0;

	return 0;
}

/**
 * b1_pool_dealloc() - deallocate memory
 * @slice:		slice to deallocate, or NULL
 *
 * This deallocates a slice that was previously allocated via b1_pool_alloc().
 * It is safe to deallocate a slice multiple times. If the slices is not
 * allocated, this is a no-op.
 */
void b1_pool_dealloc(struct b1_pool_slice *slice)
{
	struct b1_pool_slice *ps;
	struct rb_node *n;

	if (!slice || !slice->pool || WARN_ON(!slice->size))
		return;

	/*
	 * First we look for the slice logically preceding @slice. This slice
	 * must exist. In the ultimate release, there is always the root-slice
	 * remaining.
	 * We then merge the space released by @slice, as well as its trailing
	 * free space, onto the trailing free space of the preceding slice.
	 */
	n = rb_prev(&slice->rb_offset);
	WARN_ON(!n);
	ps = container_of(n, struct b1_pool_slice, rb_offset);
	if (ps->trailing)
		rb_erase(&ps->rb_trailing, &ps->pool->slices_by_trailing);
	ps->trailing += slice->size + slice->trailing;
	b1_pool_slice_link_by_trailing(ps);

	/*
	 * Secondly, with the space re-accounted on the preceding slice, we can
	 * simply unlink the slice from the management trees and clear its
	 * state.
	 */
	rb_erase(&slice->rb_offset, &slice->pool->slices_by_offset);
	if (slice->trailing > 0)
		rb_erase(&slice->rb_trailing, &slice->pool->slices_by_trailing);
	b1_pool_slice_init(slice);
}

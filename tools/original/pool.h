// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_UTIL_POOL_H
#define __B1_UTIL_POOL_H

/**
 * DOC: Pools
 *
 * A pool is a shmem-backed memory pool shared between user- and kernel-space.
 * The pool is used to transfer memory from kernel- to user-space without
 * requiring userspace to allocate memory. Instead, the shared pool can be
 * mapped by both kernel- and user-space individually.
 *
 * The pool is managed in slices. Each pool allocation is represented by a
 * non-empty, contiguous, and disjoint slice. User-space can simply mmap the
 * underlying shmem object and get access to the memory pages. Kernel-space
 * should use the provided copy-in/out helpers to move data into, or out of,
 * the shmem object.
 *
 * The pool-API provides no locking primitives. The caller is required to lock
 * around all pool management calls. Individual slices can be read and written
 * to without any locking. However, allocation and deallocation of slices must
 * be serialized.
 * The backing memory of the management structure of each slice is under full
 * control of the caller. The caller must allocate a slice object to be used
 * for a pool-allocation (the slice object can also be embedded into a
 * surrounding structure). This allows to perform pool-allocations without
 * requiring any SLAB allocations internally.
 *
 * Internally, the pool keeps an rb-tree of all allocated slices indexed by
 * their offset into the pool. Furthermore, each slice tracks its own offset
 * into the pool, its size, and the amount of free space trailing it (i.e., the
 * free space between itself and the next allocated slice). Lastly, if the
 * trailing space is non-empty, the slice is linked in another rb-tree indexed
 * by the size of the trailing space. This is used for new allocations to find
 * suitable blocks of memory to allocate a new slice in.
 */

#include <linux/kernel.h>
#include <linux/rbtree.h>
#include <linux/types.h>

struct file;
struct iovec;
struct kvec;

/**
 * B1_POOL_SLICE_SIZE_MAX - maximum size of a single slice
 *
 * A pool theoretically supports arbitrarily sized allocations. However, for
 * optimization we use u32 types for state-tracking. Hence,
 * B1_POOL_SLICE_SIZE_MAX represents the maximum size a slice can have. Right
 * now this is 2^32-1.
 *
 * If you change this, you also have to adjust the types of the internal
 * state-tracking.
 */
#define B1_POOL_SLICE_SIZE_MAX (U32_MAX)

/**
 * B1_POOL_SIZE - default size of a pool
 *
 * A pool is virtually unlimited in size. We do not intend to limit allocations
 * internally. Instead, we rely on the caller to account allocations and limit
 * them in a suitable manner. For now, we size the underlying shmem object to
 * U32_MAX, since out state-tracking uses u32-types. This is meant as
 * implementation maximum, not as recommendation for suitable limits on top.
 *
 * Note that the maximum pool-size describes the maximum linear space the
 * backing memory can provide. Fragmentation will affect the actual allocated
 * total.
 */
#define B1_POOL_SIZE (U32_MAX)

/**
 * struct b1_pool_slice - pool slice
 * @pool:		owning pool, or NULL
 * @rb_offset:		link to slice-map, indexed by offset
 * @rb_trailing:	link to slice-map, indexed by free trailing size
 * @offset:		relative offset in parent pool
 * @size:		slice size
 * @trailing:		free space after slice
 *
 * Every allocation on a pool creates a new slice. This slice represents the
 * allocation and tracks the internal state. Slices are non-empty, disjoint,
 * contiguous allocations with a fixed position in the pool. The offset of a
 * slice uniquely identifies it. The pool provides fast-lookups of slices given
 * only their offset.
 *
 * The backing memory of a slice needs to be allocated by the caller and
 * maintained as long as the slice is allocated. Once the slice is deallocated,
 * the object is under full control of the caller again (it can be destroyed
 * or re-used at the caller's discretion).
 *
 * Slices are not ref-counted. It is up to the caller to plug a ref-count on
 * top, if needed.
 *
 * Internally, as long as a slice is allocated on a pool, it tracks the space
 * it occupies, as well as all the free space trailing it. For that purpose, it
 * is linked into the following maintenance structures:
 *
 *     * offset-map: This map contains all allocated slices of a pool indexed
 *                   by their offset. This allows fast lookup of slices indexed
 *                   by their offset.
 *
 *     * trailing-map: This map contains all slices of a pool indexed by the
 *                     free space trailing them. This is a simple optimization
 *                     for new allocations to find suitable space in the pool.
 *                     A slice is not linked into this map if it has no
 *                     trailing space.
 */
struct b1_pool_slice {
	struct b1_pool *pool;
	struct rb_node rb_offset;
	struct rb_node rb_trailing;

	u32 offset;
	u32 size;
	u32 trailing;
};

#define B1_POOL_SLICE_NULL(_x) {					\
		/*							\
		 * XXX: Upstream rbtree API lacks an RB_INIT_NODE()	\
		 *      macro, hence we moved it to *_init().		\
		 *							\
		 * .rb_offset = RB_INIT_NODE((_x).rb_offset),		\
		 * .rb_trailing = RB_INIT_NODE((_x).rb_trailing),	\
		 */							\
	}

/**
 * struct b1_pool - client pool
 * @f:			backing shmem file
 * @slices_by_offset:	tree of slices, by offset
 * @slices_by_trailing:	tree of slices, by free size
 * @root_slice:		slice tracking free space of the empty pool
 *
 * A pool is used to allocate memory slices that can be shared between
 * kernel-space and user-space. A pool is always backed by a shmem-file and
 * puts a simple slice-allocator on top.
 *
 * Pools are used to transfer large sets of data to user-space, without
 * requiring a round-trip to ask user-space for a suitable memory chunk.
 * Instead, the kernel simply allocates slices in the pool and tells user-space
 * where it put the data.
 *
 * All pool operations must be serialized by the caller. No internal lock is
 * provided. Slices can be queried/modified unlocked. But any pool operation
 * (allocation, release, ...) must be serialized.
 */
struct b1_pool {
	struct file *shmem_file;
	struct rb_root slices_by_offset;
	struct rb_root slices_by_trailing;
	struct b1_pool_slice root_slice;
};

#define B1_POOL_NULL(_x) {						\
		.slices_by_offset = RB_ROOT,				\
		.slices_by_trailing = RB_ROOT,				\
		.root_slice = B1_POOL_SLICE_NULL((_x).root_slice),	\
	}

void b1_pool_slice_init(struct b1_pool_slice *slice);
void b1_pool_slice_deinit(struct b1_pool_slice *slice);

ssize_t b1_pool_slice_write_iovec(struct b1_pool_slice *slice,
				  loff_t offset,
				  struct iovec *iov,
				  size_t n_iov,
				  size_t total_len);
ssize_t b1_pool_slice_write_kvec(struct b1_pool_slice *slice,
				 loff_t offset,
				 struct kvec *iov,
				 size_t n_iov,
				 size_t total_len);

int b1_pool_init(struct b1_pool *pool, const char *filename);
void b1_pool_deinit(struct b1_pool *pool);

int b1_pool_mmap(struct b1_pool *pool, struct vm_area_struct *vma);
struct b1_pool_slice *b1_pool_find_by_offset(struct b1_pool *pool, size_t o);
int b1_pool_alloc(struct b1_pool *pool,
		  struct b1_pool_slice *slice,
		  size_t size);
void b1_pool_dealloc(struct b1_pool_slice *slice);

#endif /* __B1_UTIL_POOL_H */

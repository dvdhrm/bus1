// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_CUSTOM_H
#define __B1_CUSTOM_H

/**
 * DOC: Bus1 Custom Messages
 *
 * XXX
 */

#include <linux/kernel.h>
#include <linux/kref.h>
#include "core.h"
#include "util/flist.h"

struct b1_custom_node;
struct b1_custom_shared;
struct b1_custom_stage;
struct iov_iter;
struct iovec;
struct page;

#define B1_CUSTOM_INLINE_HANDLES (4)
#define B1_CUSTOM_INLINE_DATA (64)
#define B1_CUSTOM_INLINE_PAGES (B1_CUSTOM_INLINE_DATA / sizeof(struct page*))

struct b1_custom_node {
	struct b1_custom_shared *shared;
	size_t n_handles;
	struct b1_flist *handles;
};

struct b1_custom_shared {
	struct kref ref;
	struct b1_custom_node inline_node;

	size_t n_bytes;
	union {
		u8 inline_data[B1_CUSTOM_INLINE_DATA];
		struct page *inline_pages[B1_CUSTOM_INLINE_PAGES];
		struct b1_flist *list_pages;
	};
};

struct b1_custom_stage {
	struct b1_custom_shared *shared;

	size_t n_handles;
	size_t max_handles;
	union {
		struct b1_handle *inline_handles[B1_CUSTOM_INLINE_HANDLES];
		struct b1_flist *list_handles;
	};
};

/* nodes */

struct b1_custom_node *b1_custom_node_free(struct b1_custom_node *node);

/* shared */

struct b1_custom_shared *b1_custom_shared_new(size_t n_bytes);
void b1_custom_shared_free_internal(struct kref *ref);

int b1_custom_shared_import(struct b1_custom_shared *shared,
			    struct iov_iter *iter);

/* stages */

void b1_custom_stage_init(struct b1_custom_stage *stage);
void b1_custom_stage_deinit(struct b1_custom_stage *stage);

int b1_custom_stage_import(struct b1_custom_stage *stage,
			   size_t n_handles,
			   size_t n_bytes,
			   size_t n_vecs,
			   const struct iovec __user *u_vecs);

/* inline helpers */

static inline struct b1_custom_shared *
b1_custom_shared_ref(struct b1_custom_shared *shared)
{
	if (shared)
		kref_get(&shared->ref);
	return shared;
}

static inline struct b1_custom_shared *
b1_custom_shared_unref(struct b1_custom_shared *shared)
{
	if (shared)
		kref_put(&shared->ref, b1_custom_shared_free_internal);
	return NULL;
}

#endif /* __B1_CUSTOM_H */

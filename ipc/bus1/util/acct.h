// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_UTIL_ACCT_H
#define __B1_UTIL_ACCT_H

/**
 * DOC: Resource Accounting
 *
 * XXX
 */

#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>

enum {
	_B1_ACCT_TYPE_N,
};

struct b1_acct_share {
	unsigned int balance[_B1_ACCT_TYPE_N];
};

struct b1_acct_assets {
	unsigned int balance[_B1_ACCT_TYPE_N];
	unsigned int total[_B1_ACCT_TYPE_N];
};

struct b1_acct_usage {
	struct kref ref;
	unsigned int key;
	struct rb_node rb_resource;
	struct b1_acct_resource *resource;

	struct b1_acct_share share;
};

struct b1_acct_resource {
	struct kref ref;
	unsigned int key;
	struct rb_node rb_acct;
	struct b1_acct *acct;

	struct mutex lock;
	size_t n_usages;
	struct rb_root map_usages;
	struct b1_acct_assets assets;
};

struct b1_acct {
	struct mutex lock;
	struct rb_root map_resources;
};

struct b1_acct_charge {
	struct b1_acct_usage *usage;
	unsigned int amounts[_B1_ACCT_TYPE_N];
};

#define B1_ACCT_CHARGE_NULL {}

/* resources */

void b1_acct_resource_free_internal(struct kref *ref);
int b1_acct_resource_subscribe(struct b1_acct_resource *res,
			       struct b1_acct_charge *chargep,
			       unsigned int key);

/* accts */

void b1_acct_init(struct b1_acct *acct);
void b1_acct_deinit(struct b1_acct *acct);

struct b1_acct_resource *b1_acct_map(struct b1_acct *acct, unsigned int key);

/* charges */

void b1_acct_charge_deinit(struct b1_acct_charge *charge);

int b1_acct_charge_request(struct b1_acct_charge *charge,
			   const unsigned int (*amounts)[_B1_ACCT_TYPE_N]);
void b1_acct_charge_release(struct b1_acct_charge *charge,
			    const unsigned int (*amounts)[_B1_ACCT_TYPE_N]);
void b1_acct_charge_release_all(struct b1_acct_charge *charge);

/* inline helpers */

/**
 * b1_acct_resource_ref() - acquire reference
 * @res:		resource to operate on, or NULL
 *
 * This acquires a single reference to the given resource. If @res is NULL,
 * this is a no-op.
 *
 * Return: @res is returned.
 */
static inline struct b1_acct_resource *
b1_acct_resource_ref(struct b1_acct_resource *res)
{
	if (res)
		kref_get(&res->ref);
	return res;
}

/**
 * b1_acct_resource_unref() - release reference
 * @res:		resource to operate on, or NULL
 *
 * This releases a single reference to the given resource. If @res is NULL,
 * this is a no-op.
 *
 * Return: NULL is returned.
 */
static inline struct b1_acct_resource *
b1_acct_resource_unref(struct b1_acct_resource *res)
{
	if (res)
		kref_put_mutex(&res->ref,
			       b1_acct_resource_free_internal,
			       &res->acct->lock);
	return NULL;
}

#endif /* __B1_UTIL_ACCT_H */

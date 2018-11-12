// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/log2.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include "acct.h"

static struct b1_acct_usage *
b1_acct_usage_new(struct b1_acct_resource *res, unsigned int key)
{
	struct b1_acct_usage *usage;

	usage = kzalloc(sizeof(*usage), GFP_KERNEL);
	if (!usage)
		return ERR_PTR(-ENOMEM);

	kref_init(&usage->ref);
	usage->key = key;
	RB_CLEAR_NODE(&usage->rb_resource);
	usage->resource = res;

	return usage;
}

static void b1_acct_usage_free_internal(struct kref *ref)
{
	struct b1_acct_usage *usage = container_of(ref,
						   struct b1_acct_usage,
						   ref);
	struct b1_acct_resource *res = usage->resource;

	lockdep_assert_held(&res->lock);

	rb_erase(&usage->rb_resource, &res->map_usages);
	--res->n_usages;
	kfree(usage);

	mutex_unlock(&res->lock);
}

static struct b1_acct_usage *
b1_acct_usage_ref(struct b1_acct_usage *usage)
{
	if (usage)
		kref_get(&usage->ref);
	return usage;
}

static struct b1_acct_usage *
b1_acct_usage_unref(struct b1_acct_usage *usage)
{
	if (usage)
		kref_put_mutex(&usage->ref,
			       b1_acct_usage_free_internal,
			       &usage->resource->lock);
	return NULL;
}

static struct b1_acct_resource *
b1_acct_resource_new(struct b1_acct *acct, unsigned int key)
{
	struct b1_acct_resource *res;

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return ERR_PTR(-ENOMEM);

	kref_init(&res->ref);
	res->key = key;
	RB_CLEAR_NODE(&res->rb_acct);
	res->acct = acct;

	mutex_init(&res->lock);
	res->map_usages = RB_ROOT;

	return res;
}

/* internal callback for kref_put() */
void b1_acct_resource_free_internal(struct kref *ref)
{
	struct b1_acct_resource *res = container_of(ref,
						    struct b1_acct_resource,
						    ref);
	struct b1_acct *acct = res->acct;

	lockdep_assert_held(&acct->lock);
	WARN_ON(res->n_usages > 0);
	WARN_ON(!RB_EMPTY_ROOT(&res->map_usages));

	mutex_destroy(&res->lock);
	rb_erase(&res->rb_acct, &acct->map_resources);
	kfree(res);

	mutex_unlock(&acct->lock);
}

/**
 * b1_acct_resource_subscribe() - subscribe to a resource
 * @res:		resource to operate on
 * @chargep:		output argument for newly created charge object
 * @key:		key to subscribe as
 *
 * This creates a new subscription to the resource @res and returns it in the
 * charge objects @chargep. Any contents of @chargep are overwritten in that
 * case. The subscription will be performed with the key given as @key.
 *
 * A subscription allows performing charges on a resource. The subscription
 * pins the usage table associated with @key on the resource @res. All charges
 * that are performed via the new subscription will use this usage table.
 *
 * Return: 0 on success, negative error code on failure.
 */
int b1_acct_resource_subscribe(struct b1_acct_resource *res,
			       struct b1_acct_charge *chargep,
			       unsigned int key)
{
	struct rb_node **slot, *parent;
	struct b1_acct_usage *usage;

	mutex_lock(&res->lock);

	slot = &res->map_usages.rb_node;
	parent = NULL;
	while (*slot) {
		usage = container_of(*slot, struct b1_acct_usage, rb_resource);
		parent = *slot;

		if (key < usage->key) {
			slot = &usage->rb_resource.rb_left;
		} else if (key > usage->key) {
			slot = &usage->rb_resource.rb_right;
		} else {
			b1_acct_usage_ref(usage);
			goto done;
		}
	}

	usage = b1_acct_usage_new(res, key);
	if (!IS_ERR(usage)) {
		++res->n_usages;
		rb_link_node(&usage->rb_resource, parent, slot);
		rb_insert_color(&usage->rb_resource, &res->map_usages);
	}

done:
	mutex_unlock(&res->lock);

	if (IS_ERR(usage))
		return PTR_ERR(usage);

	*chargep = (struct b1_acct_charge) { .usage = usage };
	return 0;
}

/**
 * b1_acct_init() - initialize accounting registry
 * @acct:		registry to operate on
 *
 * This initializes the new accounting registry @acct. The caller must
 * deinitialize it when no longer needed via b1_acct_deinit().
 */
void b1_acct_init(struct b1_acct *acct)
{
	mutex_init(&acct->lock);
	acct->map_resources = RB_ROOT;
}

/**
 * b1_acct_deinit() - deinitialize accounting registry
 * @acct:		registry to operate on
 *
 * This deinitializes the accounting registry @acct, releasing all allocated
 * resources. The caller must make sure no resources are mapped, nor are there
 * any outstanding charges.
 *
 * The registry @acct must be initialized when calling this. It is an error to
 * call this multiple times.
 */
void b1_acct_deinit(struct b1_acct *acct)
{
	WARN_ON(!RB_EMPTY_ROOT(&acct->map_resources));
	mutex_destroy(&acct->lock);
}

/**
 * b1_acct_map() - map a resource object
 * @acct:		registry to operate on
 * @key:		key of resource object to map
 *
 * This tries to map the resource object with the given key. If the resource
 * object does not exist, yet, a new one is created.
 *
 * Return: Reference to resource object on success, ERR_PTR on failure.
 */
struct b1_acct_resource *b1_acct_map(struct b1_acct *acct, unsigned int key)
{
	struct rb_node **slot, *parent;
	struct b1_acct_resource *res;

	mutex_lock(&acct->lock);

	slot = &acct->map_resources.rb_node;
	parent = NULL;
	while (*slot) {
		res = container_of(*slot, struct b1_acct_resource, rb_acct);
		parent = *slot;

		if (key < res->key) {
			slot = &res->rb_acct.rb_left;
		} else if (key > res->key) {
			slot = &res->rb_acct.rb_right;
		} else {
			b1_acct_resource_ref(res);
			goto done;
		}
	}

	res = b1_acct_resource_new(acct, key);
	if (!IS_ERR(res)) {
		rb_link_node(&res->rb_acct, parent, slot);
		rb_insert_color(&res->rb_acct, &acct->map_resources);
	}

done:
	mutex_unlock(&acct->lock);
	return res;
}

/**
 * b1_acct_charge_deinit() - deinitialize charge object
 * @charge:		charge object to operate on
 *
 * This deinitializes the charge object given as @charge. Any resource charges
 * are released and a possible subscription is lifted.
 *
 * This resets the charge object to `B1_ACCT_CHARGE_NULL`. It can be reused for
 * further subscriptions.
 *
 * If the charge objects has no subscription, nor any charges (e.g., it is set
 * to `B1_ACCT_CHARGE_NULL`), then this is a no-op.
 */
void b1_acct_charge_deinit(struct b1_acct_charge *charge)
{
	if (charge->usage) {
		b1_acct_charge_release_all(charge);
		charge->usage = b1_acct_usage_unref(charge->usage);
	}
}

/**
 * b1_acct_quota() - check quotas
 * @assets:		remaining resources of this asset
 * @share:		previously acquired resources of this accounting user
 * @n_usages:		number of active accounting users on this asset
 * @amount:		amount to request
 *
 * This checks whether @amount resources can be requested from the resource
 * @assets. @share specifies how many resources the acting accounting user
 * already acquired before. @n_usages specifies how many accounting users
 * operate on @assets currently.
 *
 * The underlying algorithm allows every accounting user to acquire
 * `(n * log(n) + n)^-1` of the total resources, where `n` is the number of
 * active accounting users plus one (including the caller). This function
 * simply checks that `share + amount` does not exceed this limit. To avoid a
 * division, we rather calculate the total amount required if `n` users would
 * allocate the same amount, and then check that this does not exceed `assets`.
 *
 * With this algorithm, regardless of how many users join the system, every
 * user is guaranteed a share proportional  to `n * log(n)^2` of the total.
 * That is, a quasilinear share is guaranteed to everyone, even though we
 * cannot predict upfront how many users will request resources.
 *
 * For details and mathematical proofs, see the `r-fairdist` project, an
 * independent implementation of the "Fair Resource Distribution Algorithm"
 * which we use here.
 *
 * Return: True if the allocation is allowed, false if it exceeds any limits.
 */
static bool b1_acct_quota(unsigned int assets,
			  unsigned int share,
			  size_t n_usages,
			  unsigned int amount)
{
	unsigned int fraction, minimum, usages_plus_one;

	/*
	 * Make sure `n_usages + 1` fits into the target datatype. If it does
	 * not, there is no way the final charge wouldn't overflow as well.
	 */
	usages_plus_one = n_usages + 1;
	if (usages_plus_one <= n_usages)
		return false;

	/*
	 * ilog2() calculates the floored logarithm, but we need it ceiled. We
	 * achieve this by subtracting one from the input and adding one to the
	 * result.
	 */
	fraction = ilog2(usages_plus_one - 1) + 1;

	/*
	 * This calculates the minimum reserve for the charge @amount to
	 * succeed. See the description above for details. The formula is:
	 *
	 *     minimum = (share + amount) * (n * log_2(n) + n) - share
	 */
	if (check_mul_overflow(fraction, usages_plus_one, &fraction) ||
	    check_add_overflow(fraction, usages_plus_one, &fraction) ||
	    check_add_overflow(share, amount, &minimum) ||
	    check_mul_overflow(minimum, fraction, &minimum) ||
	    check_sub_overflow(minimum, share, &minimum))
		return false;

	return assets >= minimum;
}

/**
 * b1_acct_charge_request() - request resource charges
 * @charge:		charge to operate on
 * @amounts:		amounts to request
 *
 * This requests resource charges as specified by @amounts. The charge object
 * must be subscribed to a resource.
 *
 * This function will check quotas and resource limits before applying the
 * charge. If any quota or limit is exceeded, the charge will fail with
 * -EDQUOT.
 *
 * This function checks for integer overflows. That is, it is not possible to
 * trigger malfunction by requesting multiple charges that would overflow the
 * resource counters if combined.
 *
 * Return: 0 on success, negative error code on failure.
 */
int b1_acct_charge_request(struct b1_acct_charge *charge,
			   const unsigned int (*amounts)[_B1_ACCT_TYPE_N])
{
	struct b1_acct_assets *assets;
	struct b1_acct_share *share;
	unsigned int amount;
	size_t i;
	int r = 0;

	if (WARN_ON(!charge->usage))
		return -ENOTRECOVERABLE;

	mutex_lock(&charge->usage->resource->lock);

	share = &charge->usage->share;
	assets = &charge->usage->resource->assets;

	for (i = 0; i < ARRAY_SIZE(share->balance); ++i) {
		amount = (*amounts)[i];
		if (!amount)
			continue;

		if (b1_acct_quota(assets->balance[i],
				  share->balance[i],
				  charge->usage->resource->n_usages,
				  amount)) {
			charge->amounts[i] += amount;
			share->balance[i] += amount;
			assets->balance[i] -= amount;
			continue;
		}

		/* quota for slot @i failed; revert earlier charges and fail */
		while (i > 0) {
			--i;
			assets->balance[i] += (*amounts)[i];
			share->balance[i] -= (*amounts)[i];
			charge->amounts[i] -= (*amounts)[i];
		}
		r = -EDQUOT;
		break;
	}

	mutex_unlock(&charge->usage->resource->lock);

	return r;
}

/**
 * b1_acct_charge_release() - release charges
 * @charge:		charge to operate on
 * @amounts:		amounts to release
 *
 * This releases charges as specified in @amounts. The caller is responsible to
 * guarantee that the charges were previously acquired through
 * b1_acct_charge_request().
 *
 * It is safe to split charges after they were acquired through
 * b1_acct_charge_request(). That is, a single request might be released by
 * multiple calls to b1_acct_charge_release(). However, the caller must make
 * sure the numbers add up.
 */
void b1_acct_charge_release(struct b1_acct_charge *charge,
			    const unsigned int (*amounts)[_B1_ACCT_TYPE_N])
{
	struct b1_acct_assets *assets;
	struct b1_acct_share *share;
	size_t i;

	if (WARN_ON(!charge->usage))
		return;

	mutex_lock(&charge->usage->resource->lock);

	share = &charge->usage->share;
	assets = &charge->usage->resource->assets;

	for (i = 0; i < ARRAY_SIZE(share->balance); ++i) {
		if (WARN_ON((*amounts)[i] > charge->amounts[i]))
			continue;

		/*
		 * NOTE: @amounts might point to @charge->amounts. We
		 *       need to be careful not to overwrite it too
		 *       early.
		 */
		assets->balance[i] += (*amounts)[i];
		share->balance[i] -= (*amounts)[i];
		charge->amounts[i] -= (*amounts)[i];
	}

	mutex_unlock(&charge->usage->resource->lock);
}

/**
 * b1_acct_charge_release_all() - release all charges
 * @charge:		charge to operate on
 *
 * This releases all charges that are left on the charge object @charge.
 *
 * If @charge is not subscribed to a resource, this is a no-op.
 */
void b1_acct_charge_release_all(struct b1_acct_charge *charge)
{
	if (charge->usage)
		b1_acct_charge_release(charge, &charge->amounts);
}

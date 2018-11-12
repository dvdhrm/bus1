// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/rbtree.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include "distq.h"
#include "util.h"

static bool b1_distq_ts_committed(s64 ts)
{
	/*
	 * We use 64bit clocks which increment by 2 per tick. We start with 0
	 * and as such a clock is always even numbered. For every tick you can
	 * mark the timestamp as "committed" by setting the LSB. A committed
	 * timestamp is thus always higher than its originating clock value and
	 * denotes timestamps that are settled and no longer increase.
	 */
	return ts & S64_C(0x1);
}

static void b1_distq_ts_force_sync(atomic64_t *ts, s64 to)
{
	s64 v;

	/*
	 * This synchronizes the timestamp value @ts with @to. That is, it
	 * increases @ts atomically to @to, unless it is already bigger than
	 * @to. Note that this synchronization is forced, so @ts cannot be
	 * marked as committed (which would mean it cannot change anymore).
	 */

	B1_WARN_ON(b1_distq_ts_committed(to));

	/*
	 * We do not provide any explicit ordering here, as forced
	 * synchronizations are purely local and do not convey any state
	 * change.
	 */
	v = atomic64_read(ts);
	do {
		if (v >= to || B1_WARN_ON(b1_distq_ts_committed(v)))
			return;
	} while (!atomic64_try_cmpxchg_relaxed(ts, &v, to));
}

static s64 b1_distq_ts_try_sync(atomic64_t *ts, s64 to)
{
	s64 v;

	/*
	 * This tries to synchronize @ts with @to. Similarly to
	 * b1_distq_ts_force_sync(), this tries to increase @ts to at least @to
	 * (unless it is already greater than @to). However, if @ts is marked
	 * as committed, this will not perform the synchronization.
	 *
	 * In all cases, this function returns the value of @ts after the
	 * operation.
	 */

	B1_WARN_ON(b1_distq_ts_committed(to));

	/*
	 * No explicit ordering here, since the actual value of a timestamp
	 * does not convey a state-change. If other data is associated with a
	 * specific timestamp, they must synchronize themselves.
	 */
	v = atomic64_read(ts);
	do {
		if (v >= to || b1_distq_ts_committed(v))
			return v;
	} while (!atomic64_try_cmpxchg_relaxed(ts, &v, to));

	return to;
}

static int b1_distq_node_compare(struct b1_distq_node *a,
				 struct b1_distq_node *b)
{
	if (a->timestamp < b->timestamp)
		return -1;
	else if (a->timestamp > b->timestamp)
		return 1;
	else if ((unsigned long)a->tx < (unsigned long)b->tx)
		return -1;
	else if ((unsigned long)a->tx > (unsigned long)b->tx)
		return 1;
	else if ((unsigned long)a < (unsigned long)b)
		return -1;
	else if ((unsigned long)a > (unsigned long)b)
		return 1;
	else
		return 0;
}

/**
 * b1_distq_node_init() - XXX
 */
void b1_distq_node_init(struct b1_distq_node *node)
{
	*node = (struct b1_distq_node){
		.n_refs = REFCOUNT_INIT(0),
	};
	RB_CLEAR_NODE(&node->rb_ready);
}

/**
 * b1_distq_node_deinit() - XXX
 */
void b1_distq_node_deinit(struct b1_distq_node *node)
{
	B1_WARN_ON(refcount_read(&node->n_refs) != 0);
	B1_WARN_ON(node->tx);
	B1_WARN_ON(node->next_queue);
	B1_WARN_ON(!RB_EMPTY_NODE(&node->rb_ready));
}

/**
 * b1_distq_node_claim() - XXX
 */
void b1_distq_node_claim(struct b1_distq_node *node)
{
	WARN_ON(refcount_read(&node->n_refs) != 0);
	refcount_set(&node->n_refs, 1);
}

/**
 * b1_distq_node_finalize() - XXX
 */
struct b1_distq_tx *b1_distq_node_finalize(struct b1_distq_node *node)
{
	struct b1_distq_tx *tx;

	tx = node->tx;
	node->tx = NULL;
	return tx;
}

/**
 * b1_distq_node_queue() - XXX
 */
void b1_distq_node_queue(struct b1_distq_node *node,
			 struct b1_distq_tx *tx,
			 struct b1_distq_peer *dest)
{
	struct b1_distq_node *head;

	if (B1_WARN_ON(node->tx || node->next_queue))
		return;

	refcount_inc(&node->n_refs);
	refcount_inc(&tx->n_refs);
	node->tx = tx;

	/*
	 * Link @node into the unlocked incoming queue of @dest. We use an
	 * unlocked single-linked list, but allow closing a queue. The list
	 * uses B1_TAIL as tail-pointer, and NULL to mark a queue as closed.
	 *
	 * We use a cmpxchg-loop to replace the front-pointer of the list with
	 * @node. Note that as soon as the entry is linked, the receiver might
	 * dequeue it. Thus, this function transfers the object to the
	 * destination with immediate effect. The cmpxchg() provides the
	 * necessary barriers and pairs with the xchg() on the receive side.
	 *
	 * If the queue is closed, it means the owner destroyed all its
	 * objects, finished all transactions, and finalized the queue. This
	 * implies that all destructions are settled, so any further operation
	 * will end up with a higher timestamp. Therefore, instead of queuing
	 * an entry (which would never be dequeued, anymore), we just never
	 * queue it (thus we emulate an immediate dequeue+discard).
	 */
	head = READ_ONCE(dest->queue.incoming);
	do {
		if (unlikely(!head)) {
			node->next_queue = NULL;
			B1_WARN_ON(refcount_dec_and_test(&node->n_refs));
			return;
		}
		node->next_queue = head;
	} while ((head = cmpxchg(&dest->queue.incoming,
				 node->next_queue,
				 node)) != node->next_queue);

	/*
	 * The cmpxchg() guarantees the node is visible to the other side
	 * before we check their clock.
	 */
	b1_distq_ts_force_sync(&tx->timestamp, atomic64_read(&dest->clock));
}

/**
 * b1_distq_node_commit() - XXX
 */
void b1_distq_node_commit(struct b1_distq_node *node,
			  struct b1_distq_peer *dest)
{
	s64 ts;

	if (B1_WARN_ON(!node->tx))
		return;

	/*
	 * Notify the message receiver of the new message. The wake-up
	 * guarantees that *if* the other side is queued on the wait-queue,
	 * they will either see all writes to the committed message (including
	 * the change to @n_committed), or they will be woken up.
	 *
	 * Additionally, we make sure the commit-timestamp is visible before
	 * the change to @n_committed is. This is not strictly necessary to
	 * guarantee wakeups, but it prevents the peer from being marked
	 * readable without any message ready (i.e., we prevent @n_committed
	 * from being >0 but the message-timestamp still not committed). This
	 * is paired with the ACQUIRE on the readers of @n_committed. Note that
	 * "happens-before" is transitive, so it does not matter which thread
	 * performs the wake-up in case @n_committed is ever negative due to
	 * messages being received early.
	 */
	if (atomic_inc_return_release(&dest->n_committed) > 0)
		wake_up(&dest->waitq);

	/*
	 * We now synchronize the remote clock with the timestamp of the
	 * message. Note that every peer does that on receival of a message as
	 * well. However, we explicitly synchronize early to minimize the
	 * chances that side-channel communication surpasses us:
	 *
	 *     Imagine a multicast receiver notifying an independent peer via a
	 *     side-channel of the multicast message. This independent peer now
	 *     messages another receiver of the original multicast. If that
	 *     other receiver did not dequeue the multicast, yet, then this new
	 *     message is not ordered at all against the multicast.
	 *     If we synchronize clocks of all receivers during SEND, we make
	 *     sure side-channels are ordered and this race does not appear.
	 *
	 * For now, we do not synchronize clocks under a lock. That is,
	 * side-channel ordering is *not* guaranteed. All we do is minimize the
	 * chances of unordered messages. If we want to guarantee side-channel
	 * ordering, we have to prevent peers to receive messages until all
	 * receiver clocks are synchronized. This will require a message lock
	 * since clock-synchronization is done with a committed transaction.
	 *
	 * Long story short: This clock synchronization is optional and only
	 *                   provided to improve side-channel ordering.
	 */
	ts = atomic64_read(&node->tx->timestamp) + 1;
	b1_distq_ts_force_sync(&dest->clock, ts);
}

/**
 * b1_distq_tx_init() - XXX
 */
void b1_distq_tx_init(struct b1_distq_tx *tx)
{
	*tx = (struct b1_distq_tx){
		.n_refs = REFCOUNT_INIT(0),
		.timestamp = ATOMIC64_INIT(0),
	};
}

/**
 * b1_distq_tx_deinit() - XXX
 */
void b1_distq_tx_deinit(struct b1_distq_tx *tx)
{
	B1_WARN_ON(refcount_read(&tx->n_refs) != 0);
}

/**
 * b1_distq_tx_claim() - XXX
 */
void b1_distq_tx_claim(struct b1_distq_tx *tx)
{
	WARN_ON(refcount_read(&tx->n_refs) != 0);
	refcount_set(&tx->n_refs, 1);
}

/**
 * b1_distq_tx_commit() - XXX
 */
void b1_distq_tx_commit(struct b1_distq_tx *tx, struct b1_distq_peer *sender)
{
	s64 ts;

	/* XXX: Is sender-sync really necessary? */
	ts = atomic64_read(&sender->clock);
	b1_distq_ts_force_sync(&tx->timestamp, ts);

	/*
	 * Commit the message by marking the commit-timestamp. Note that this
	 * means the timestamp is frozen from now on, no other modifications
	 * can happen on a committed timestamp.
	 * Marking the timestamp as committed only settles the timestamp, but
	 * does not order against other operations so no barriers are needed.
	 */
	atomic64_inc(&tx->timestamp);
}

/**
 * b1_distq_peer_init() - XXX
 */
void b1_distq_peer_init(struct b1_distq_peer *peer)
{
	*peer = (struct b1_distq_peer){
		.clock = ATOMIC64_INIT(0),
		.n_committed = ATOMIC_INIT(0),
		.waitq = __WAIT_QUEUE_HEAD_INITIALIZER(peer->waitq),
		.queue.incoming = B1_TAIL,
		.queue.busy = B1_TAIL,
	};
	peer->queue.ready = RB_ROOT;
}

/**
 * b1_distq_peer_deinit() - XXX
 */
void b1_distq_peer_deinit(struct b1_distq_peer *peer)
{
	/*
	 * We do not verify @peer->n_committed, since it can be non-zero when
	 * entries are committed, which have been queued after a queue was
	 * finalized. In this case the counter has no meaning anymore, so we
	 * can simply ignore it.
	 */

	B1_WARN_ON(peer->queue.incoming && peer->queue.incoming != B1_TAIL);
	B1_WARN_ON(peer->queue.busy && peer->queue.busy != B1_TAIL);
	B1_WARN_ON(!RB_EMPTY_ROOT(&peer->queue.ready));
	B1_WARN_ON(peer->queue.ready_first);
	B1_WARN_ON(peer->queue.ready_last);
}

/**
 * b1_distq_peer_finalize() - XXX
 */
struct b1_distq_node *b1_distq_peer_finalize(struct b1_distq_peer *peer)
{
	struct b1_distq_node *node, *safe, **slot, *list = B1_TAIL;

	/* fetch incoming queue */
	list = xchg(&peer->queue.incoming, NULL);
	if (!list)
		return B1_TAIL;

	/* prepend the busy queue */
	slot = &peer->queue.busy;
	while (*slot != B1_TAIL)
		slot = &(*slot)->next_queue;
	*slot = list;
	list = peer->queue.busy;
	peer->queue.busy = NULL;

	/* prepend all ready items */
	rbtree_postorder_for_each_entry_safe(node, safe,
					     &peer->queue.ready, rb_ready) {
		RB_CLEAR_NODE(&node->rb_ready);
		node->next_queue = list;
		list = node;
	}
	peer->queue.ready = RB_ROOT;
	peer->queue.ready_first = NULL;
	peer->queue.ready_last = NULL;

	return list;
}

static void b1_distq_peer_push_ready(struct b1_distq_peer *peer,
				     struct b1_distq_node *node)
{
	struct b1_distq_node *e;
	struct rb_node **slot, *parent;
	bool leftmost = true, rightmost = true;
	int r;

	slot = &peer->queue.ready.rb_node;
	parent = NULL;
	while (*slot) {
		parent = *slot;
		e = container_of(*slot, struct b1_distq_node, rb_ready);
		r = b1_distq_node_compare(node, e);
		if (r < 0) {
			slot = &parent->rb_left;
			rightmost = false;
		} else /* if (r >= 0) */ {
			slot = &parent->rb_right;
			leftmost = false;
		}
	}

	rb_link_node(&node->rb_ready, parent, slot);
	rb_insert_color(&node->rb_ready, &peer->queue.ready);

	if (leftmost)
		peer->queue.ready_first = node;
	if (rightmost)
		peer->queue.ready_last = node;
}

static struct b1_distq_node *
b1_distq_peer_pop_ready(struct b1_distq_peer *peer)
{
	struct b1_distq_node *node = peer->queue.ready_first;

	if (!node)
		return NULL;

	if (node == peer->queue.ready_last) {
		peer->queue.ready_first = NULL;
		peer->queue.ready_last = NULL;
		peer->queue.ready = RB_ROOT;
	} else {
		peer->queue.ready_first =
			container_of(rb_next(&node->rb_ready),
				     struct b1_distq_node,
				     rb_ready);
		rb_erase(&node->rb_ready, &peer->queue.ready);
	}

	RB_CLEAR_NODE(&node->rb_ready);
	return node;
}

static void b1_distq_peer_sync(struct b1_distq_peer *peer, s64 to)
{
	struct b1_distq_node *node, **slot;
	size_t i;
	s64 ts;

	if (B1_WARN_ON(b1_distq_ts_committed(to) || to <= peer->local))
		return;

	peer->local = to;
	b1_distq_ts_force_sync(&peer->clock, to);

	slot = &peer->queue.busy;
	for (i = 0; i < 2; ++i) {
		while ((node = *slot) != B1_TAIL) {
			B1_WARN_ON(!node);
			ts = b1_distq_ts_try_sync(&node->tx->timestamp, to);
			if (b1_distq_ts_committed(ts)) {
				*slot = node->next_queue;
				node->next_queue = NULL;
				node->timestamp = node->timestamp ?: ts;
				b1_distq_peer_push_ready(peer, node);
			} else {
				slot = &node->next_queue;
			}
		}

		if (!i)
			*slot = xchg(&peer->queue.incoming, B1_TAIL);
	}
}

static void b1_distq_peer_prefetch(struct b1_distq_peer *peer)
{
	struct b1_distq_node *node, **slot;
	size_t i;
	s64 ts;

	slot = &peer->queue.busy;
	for (i = 0; i < 2; ++i) {
		while ((node = *slot) != B1_TAIL) {
			B1_WARN_ON(!node);
			ts = atomic64_read(&node->tx->timestamp);
			if (b1_distq_ts_committed(ts)) {
				*slot = node->next_queue;
				node->next_queue = NULL;
				node->timestamp = node->timestamp ?: ts;
				b1_distq_peer_push_ready(peer, node);
			} else {
				slot = &node->next_queue;
			}
		}

		if (!i)
			*slot = xchg(&peer->queue.incoming, B1_TAIL);
	}
}

/**
 * b1_distq_peer_peek() - peek at the queue front
 * @peer:			peer to operate on
 *
 * The caller must serialize access to this function as if they hold a write
 * lock on @peer.
 *
 * This returns a pointer to the front of the queue of @peer. The returned node
 * is valid until the caller calls this function again, drops the node via
 * b1_distq_peer_pop(), finalizes the queue via b1_distq_peer_finalize(), or
 * drops the semantical write-lock on @peer (whichever happens first).
 *
 * This function performs queue maintenance if the front entry is a new front
 * entry. That is, if this function is called multiple times without dropping
 * the front entry, this function operates in O(1). However, for every new
 * queue front (i.e., once for every incoming message) the queue must resolve
 * possible ordering issues against any other incoming message. In the
 * fast-path (and ideal case), this function simply fetches all pending
 * messages from the incoming queue, sorts them by their commit-timestamp, and
 * prepares them in the ready-queue for retrieval by the caller. This is done
 * in constant time for each message (or O(n) for 'n' incoming messages).
 * Therefore, retrieval of messages usually takes O(1).
 *
 * Now there is the special case where multiple CPUs race each other and queue
 * nodes on the same destination. If those happen to be part of bigger
 * transactions, those transactions must be settled.
 *
 * Return: Pointer to the front element is returned, or NULL if none.
 */
struct b1_distq_node *b1_distq_peer_peek(struct b1_distq_peer *peer)
{
	struct b1_distq_node *first;
	s64 ts;

	first = peer->queue.ready_first;
	if (!first) {
		/*
		 * We have no messages in the ready-queue, but there might be
		 * committed messages in our incoming queue. We now have to
		 * walk the incoming queue and move all the committed messages
		 * into the ready-queue.
		 */
		b1_distq_peer_prefetch(peer);

		first = peer->queue.ready_first;
		if (!first)
			return NULL;
	}

	if (first->timestamp >= peer->local) {
		/*
		 * We have an entry to return, but we have not yet synchronized
		 * our local clock with it. Hence, there can be entries in the
		 * incoming queue that might eventually order before our queue
		 * front. Hence, we have to synchronize the incoming queue to
		 * resolve all conflicts. We use the ready-queue-tail for this
		 * to make sure our entire ready-queue is synchronized.
		 */
		ts = peer->queue.ready_last->timestamp + 1;
		b1_distq_peer_sync(peer, ts);

		first = peer->queue.ready_first;
		B1_WARN_ON(!first);
	}

	return first;
}

/**
 * b1_distq_peer_pop() - drop node from a queue
 * @peer:			peer to operate on
 * @node:			node to drop
 *
 * This drops the node given as @node from the queue of @peer. The caller must
 * guarantee that @node was retrieved via a previous call to
 * b1_distq_peer_peek(). That is, the front of the queue is the only element
 * that can be dropped directly.
 *
 * Once this call returns, @node is no longer valid.
 *
 * The caller must serialize access to this function as if they hold a write
 * lock on @peer.
 */
void b1_distq_peer_pop(struct b1_distq_peer *peer, struct b1_distq_node *node)
{
	struct b1_distq_node *popped;

	popped = b1_distq_peer_pop_ready(peer);
	B1_WARN_ON(node != popped);

	/*
	 * Decrement the commit counter. No ordering necessary, since this is
	 * always synchronized through serialized access to @peer. Note that
	 * this might put the counter <0 in case we retrieve a message before
	 * the sender synchronized the receivers.
	 */
	atomic_dec(&peer->n_committed);
}

// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_UTIL_DISTQ_H
#define __B1_UTIL_DISTQ_H

/**
 * DOC: Distributed Queues
 *
 * XXX
 */

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/rbtree.h>
#include <linux/refcount.h>
#include <linux/wait.h>

struct b1_distq_node;
struct b1_distq_peer;
struct b1_distq_tx;

struct b1_distq_node {
	refcount_t n_refs;
	unsigned int userdata;
	s64 timestamp;
	struct b1_distq_tx *tx;
	struct b1_distq_node *next_queue;
	struct rb_node rb_ready;
};

struct b1_distq_tx {
	refcount_t n_refs;
	atomic64_t timestamp;
};

struct b1_distq_peer {
	atomic64_t clock;
	s64 local;
	atomic_t n_committed;
	wait_queue_head_t waitq;
	struct {
		struct b1_distq_node *incoming;
		struct b1_distq_node *busy;
		struct rb_root ready;
		struct b1_distq_node *ready_first;
		struct b1_distq_node *ready_last;
	} queue;
};

/* nodes */

void b1_distq_node_init(struct b1_distq_node *node);
void b1_distq_node_deinit(struct b1_distq_node *node);
void b1_distq_node_claim(struct b1_distq_node *node);

struct b1_distq_tx *b1_distq_node_finalize(struct b1_distq_node *node);

void b1_distq_node_queue(struct b1_distq_node *node,
			 struct b1_distq_tx *tx,
			 struct b1_distq_peer *dest);
void b1_distq_node_commit(struct b1_distq_node *node,
			  struct b1_distq_peer *dest);

/* transactions */

void b1_distq_tx_init(struct b1_distq_tx *tx);
void b1_distq_tx_deinit(struct b1_distq_tx *tx);
void b1_distq_tx_claim(struct b1_distq_tx *tx);

void b1_distq_tx_commit(struct b1_distq_tx *tx, struct b1_distq_peer *sender);

/* peers */

void b1_distq_peer_init(struct b1_distq_peer *peer);
void b1_distq_peer_deinit(struct b1_distq_peer *peer);

struct b1_distq_node *b1_distq_peer_finalize(struct b1_distq_peer *peer);
struct b1_distq_node *b1_distq_peer_peek(struct b1_distq_peer *peer);
void b1_distq_peer_pop(struct b1_distq_peer *peer, struct b1_distq_node *node);

/**
 * b1_distq_peer_poll() - query queue for readiness
 * @peer:			peer to operate on
 *
 * This checks whether there are entries ready to be retrieved from @peer. If
 * there are, the next b1_distq_peer_peek() is guaranteed to return a valid
 * entry.
 *
 * Return: True if there are entries ready, false if not.
 */
static inline bool b1_distq_peer_poll(struct b1_distq_peer *peer)
{
	/*
	 * We ACQUIRE @n_committed. This makes sure that if the commit-counter
	 * was increased, we also see the commit timestamp on the respective
	 * message. This is paired with the RELEASE on the send-side.
	 */
	return atomic_read_acquire(&peer->n_committed) > 0;
}

#endif /* __B1_UTIL_DISTQ_H */

// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include "test.h"
#include "util/distq.h"
#include "util/util.h"

static void b1_test_distq_basic_peer(void)
{
	struct b1_distq_peer peer;
	struct b1_distq_node *l;

	/* test simple init+deinit sequence */
	b1_distq_peer_init(&peer);
	b1_distq_peer_deinit(&peer);

	/* test init+deinit sequence with multiple finalizations */
	b1_distq_peer_init(&peer);
	l = b1_distq_peer_finalize(&peer);
	WARN_ON(l != B1_TAIL);
	l = b1_distq_peer_finalize(&peer);
	WARN_ON(l != B1_TAIL);
	b1_distq_peer_deinit(&peer);

	/* verify the queue is empty */
	b1_distq_peer_init(&peer);
	WARN_ON(b1_distq_peer_poll(&peer));
	l = b1_distq_peer_peek(&peer);
	WARN_ON(l);
	l = b1_distq_peer_finalize(&peer);
	WARN_ON(l != B1_TAIL);
	b1_distq_peer_deinit(&peer);
}

static void b1_test_distq_basic_tx(void)
{
	struct b1_distq_peer peer;
	struct b1_distq_tx tx;

	/* test simple init+deinit sequence */
	b1_distq_tx_init(&tx);
	b1_distq_tx_deinit(&tx);

	/* test committing an empty transaction */
	b1_distq_peer_init(&peer);
	b1_distq_tx_init(&tx);
	b1_distq_tx_commit(&tx, &peer);
	WARN_ON(atomic64_read(&tx.timestamp) != 1);
	b1_distq_tx_deinit(&tx);
	b1_distq_peer_deinit(&peer);
}

static void b1_test_distq_basic_node(void)
{
	struct b1_distq_node node;

	/* test simple init+deinit sequence */
	b1_distq_node_init(&node);
	b1_distq_node_deinit(&node);

	/* verify tx is unset if never queued */
	b1_distq_node_init(&node);
	WARN_ON(b1_distq_node_finalize(&node));
	b1_distq_node_deinit(&node);
}

static void b1_test_distq_unicast_isolated(void)
{
	struct b1_distq_peer p1, p2;
	struct b1_distq_tx tx;
	struct b1_distq_node node;

	/*
	 * Test sending a single unicast from @p1 to @p2. All objects live on
	 * the stack and we track their ref-counts to verify ownership is
	 * handed on correctly.
	 */

	/* initialize all objects */
	b1_distq_peer_init(&p1);
	b1_distq_peer_init(&p2);
	b1_distq_tx_init(&tx);
	b1_distq_tx_claim(&tx);
	b1_distq_node_init(&node);
	b1_distq_node_claim(&node);

	/* queue the node and verify it is queued */
	b1_distq_node_queue(&node, &tx, &p2);
	WARN_ON(node.tx != &tx);
	WARN_ON(!node.next_queue);
	WARN_ON(!RB_EMPTY_NODE(&node.rb_ready));
	WARN_ON(b1_distq_peer_poll(&p2));
	WARN_ON(b1_distq_peer_peek(&p2));

	/* commit the transaction and verify its timestamp */
	b1_distq_tx_commit(&tx, &p1);
	WARN_ON(atomic64_read(&tx.timestamp) != 1);

	/* commit the node and verify it is queued */
	b1_distq_node_commit(&node, &p2);
	WARN_ON(!node.next_queue);
	WARN_ON(!RB_EMPTY_NODE(&node.rb_ready));
	WARN_ON(!b1_distq_peer_poll(&p2));
	WARN_ON(atomic64_read(&p2.clock) != 2);

	/* fetch the incoming queue and verify it is the correct node */
	WARN_ON(&node != b1_distq_peer_peek(&p2));
	WARN_ON(node.next_queue);
	WARN_ON(RB_EMPTY_NODE(&node.rb_ready));

	/* drop from the queue */
	b1_distq_peer_pop(&p2, &node);
	WARN_ON(node.next_queue);
	WARN_ON(!RB_EMPTY_NODE(&node.rb_ready));
	WARN_ON(&tx != b1_distq_node_finalize(&node));
	WARN_ON(refcount_dec_and_test(&tx.n_refs));
	WARN_ON(refcount_dec_and_test(&node.n_refs));

	/* deinitialize everything */
	WARN_ON(!refcount_dec_and_test(&node.n_refs));
	WARN_ON(!refcount_dec_and_test(&tx.n_refs));
	b1_distq_node_deinit(&node);
	b1_distq_tx_deinit(&tx);
	b1_distq_peer_deinit(&p2);
	b1_distq_peer_deinit(&p1);
}

static void b1_test_distq_unicast_contested(void)
{
	struct b1_distq_peer peer;
	struct b1_distq_tx tx1, tx2;
	struct b1_distq_node n1, n2, *l;

	/*
	 * Test sending two unicasts to @peer. All objects live on the stack
	 * and we track their ref-counts to verify ownership is handed on
	 * correctly. Note that we do not employ atomic unicasts, but pretend
	 * they are part of a bigger transaction. As such they are queued
	 * before they are committed. Thus, we simulate a conflict between two
	 * nodes and verify it is resolved correctly.
	 */

	/* initialize all objects */
	b1_distq_peer_init(&peer);
	b1_distq_tx_init(&tx1);
	b1_distq_tx_init(&tx2);
	b1_distq_tx_claim(&tx1);
	b1_distq_tx_claim(&tx2);
	b1_distq_node_init(&n1);
	b1_distq_node_init(&n2);
	b1_distq_node_claim(&n1);
	b1_distq_node_claim(&n2);

	/* queue both nodes */
	b1_distq_node_queue(&n1, &tx1, &peer);
	b1_distq_node_queue(&n2, &tx2, &peer);
	WARN_ON(b1_distq_peer_poll(&peer));

	/* commit @n1 */
	b1_distq_tx_commit(&tx1, &peer);
	b1_distq_node_commit(&n1, &peer);
	WARN_ON(atomic64_read(&tx1.timestamp) != 1);
	WARN_ON(atomic64_read(&tx2.timestamp) != 0);
	WARN_ON(atomic64_read(&peer.clock) != 2);
	WARN_ON(!b1_distq_peer_poll(&peer));

	/* retrieve @n1 and verify the conflict was resolved */
	WARN_ON(&n1 != b1_distq_peer_peek(&peer));
	WARN_ON(atomic64_read(&tx1.timestamp) != 1);
	WARN_ON(atomic64_read(&tx2.timestamp) != 2);
	WARN_ON(atomic64_read(&peer.clock) != 2);

	/* commit @n2 */
	b1_distq_tx_commit(&tx2, &peer);
	b1_distq_node_commit(&n2, &peer);
	WARN_ON(atomic64_read(&tx1.timestamp) != 1);
	WARN_ON(atomic64_read(&tx2.timestamp) != 3);
	WARN_ON(atomic64_read(&peer.clock) != 4);

	/* finalize the peer */
	l = b1_distq_peer_finalize(&peer);
	if (l == &n1) {
		l = l->next_queue;
		n1.next_queue = NULL;

		WARN_ON(l != &n2);
		WARN_ON(l->next_queue != B1_TAIL);
		n2.next_queue = NULL;
	} else {
		WARN_ON(l != &n2);
		l = l->next_queue;
		n2.next_queue = NULL;

		WARN_ON(l != &n1);
		WARN_ON(l->next_queue != B1_TAIL);
		n1.next_queue = NULL;
	}
	WARN_ON(&tx2 != b1_distq_node_finalize(&n2));
	WARN_ON(&tx1 != b1_distq_node_finalize(&n1));
	WARN_ON(refcount_dec_and_test(&tx2.n_refs));
	WARN_ON(refcount_dec_and_test(&tx1.n_refs));
	WARN_ON(refcount_dec_and_test(&n2.n_refs));
	WARN_ON(refcount_dec_and_test(&n1.n_refs));

	/* deinitialize everything */
	WARN_ON(!refcount_dec_and_test(&n2.n_refs));
	WARN_ON(!refcount_dec_and_test(&n1.n_refs));
	WARN_ON(!refcount_dec_and_test(&tx2.n_refs));
	WARN_ON(!refcount_dec_and_test(&tx1.n_refs));
	b1_distq_node_deinit(&n2);
	b1_distq_node_deinit(&n1);
	b1_distq_tx_deinit(&tx2);
	b1_distq_tx_deinit(&tx1);
	b1_distq_peer_deinit(&peer);
}

/**
 * b1_test_distq() - run distq tests
 *
 * This runs all internal tests of the distq subsystem.
 */
void b1_test_distq(void)
{
	b1_test_distq_basic_peer();
	b1_test_distq_basic_tx();
	b1_test_distq_basic_node();
	b1_test_distq_unicast_isolated();
	b1_test_distq_unicast_contested();
}

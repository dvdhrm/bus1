// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/wait.h>
#include <uapi/linux/bus1.h>
#include "core.h"
#include "custom.h"
#include "uapi.h"
#include "util/util.h"

static u64 b1_uapi_next_id(u64 *seq)
{
	return (++*seq) << 1;
}

/**
 * b1_uapi_handle_init() - XXX
 */
void b1_uapi_handle_init(struct b1_uapi_handle *handle)
{
	*handle = (struct b1_uapi_handle){};
	RB_CLEAR_NODE(&handle->rb_peer);
}

/**
 * b1_uapi_handle_deinit() - XXX
 */
void b1_uapi_handle_deinit(struct b1_uapi_handle *handle)
{
	B1_WARN_ON(!RB_EMPTY_NODE(&handle->rb_peer));
	B1_WARN_ON(handle->n_public);
	B1_WARN_ON(handle->op.next);
	B1_WARN_ON(handle->op.n);
}

static u64 b1_uapi_handle_open(struct b1_uapi_handle *handle)
{
	struct b1_uapi_peer *peer = &b1_handle_from_uapi(handle)->owner->uapi;
	struct b1_uapi_handle *n;
	struct rb_node **slot, *parent;

	b1_assert_held(&peer->lock);

	if (!handle->id)
		handle->id = b1_uapi_next_id(&peer->id_allocator);

	if (RB_EMPTY_NODE(&handle->rb_peer)) {
		parent = NULL;
		slot = &peer->map_handles.rb_node;
		while (*slot) {
			n = container_of(*slot, struct b1_uapi_handle, rb_peer);
			parent = *slot;
			if (handle->id < n->id)
				slot = &(*slot)->rb_left;
			else /* if (handle->id >= n->id) */
				slot = &(*slot)->rb_left;
		}

		rb_link_node(&handle->rb_peer, parent, slot);
		rb_insert_color(&handle->rb_peer, &peer->map_handles);
		b1_handle_ref(b1_handle_from_uapi(handle));
	}

	++handle->n_public;
	return handle->id;
}

static void b1_uapi_handle_close(struct b1_uapi_handle *handle)
{
	struct b1_uapi_peer *peer = &b1_handle_from_uapi(handle)->owner->uapi;

	b1_assert_held(&peer->lock);

	if (!--handle->n_public) {
		if (!RB_EMPTY_NODE(&handle->rb_peer)) {
			rb_erase(&handle->rb_peer, &peer->map_handles);
			RB_CLEAR_NODE(&handle->rb_peer);
			b1_handle_unref(b1_handle_from_uapi(handle));
		}
	}
}

/**
 * b1_uapi_object_init() - XXX
 */
void b1_uapi_object_init(struct b1_uapi_object *object)
{
	*object = (struct b1_uapi_object){};
	RB_CLEAR_NODE(&object->rb_peer);
}

/**
 * b1_uapi_object_deinit() - XXX
 */
void b1_uapi_object_deinit(struct b1_uapi_object *object)
{
	B1_WARN_ON(!RB_EMPTY_NODE(&object->rb_peer));
	B1_WARN_ON(object->op.next);
}

static u64 b1_uapi_object_open(struct b1_uapi_object *object)
{
	struct b1_uapi_peer *peer = &b1_object_from_uapi(object)->owner->uapi;
	struct b1_uapi_object *n;
	struct rb_node **slot, *parent;

	b1_assert_held(&peer->lock);

	if (!object->id)
		object->id = b1_uapi_next_id(&peer->id_allocator);

	if (RB_EMPTY_NODE(&object->rb_peer)) {
		parent = NULL;
		slot = &peer->map_objects.rb_node;
		while (*slot) {
			n = container_of(*slot, struct b1_uapi_object, rb_peer);
			parent = *slot;
			if (object->id < n->id)
				slot = &(*slot)->rb_left;
			else /* if (object->id >= n->id) */
				slot = &(*slot)->rb_left;
		}

		rb_link_node(&object->rb_peer, parent, slot);
		rb_insert_color(&object->rb_peer, &peer->map_objects);
		b1_object_ref(b1_object_from_uapi(object));
	}

	return object->id;
}

static void b1_uapi_object_close(struct b1_uapi_object *object)
{
	struct b1_uapi_peer *peer = &b1_object_from_uapi(object)->owner->uapi;

	b1_assert_held(&peer->lock);

	if (!RB_EMPTY_NODE(&object->rb_peer)) {
		rb_erase(&object->rb_peer, &peer->map_objects);
		RB_CLEAR_NODE(&object->rb_peer);
		b1_object_unref(b1_object_from_uapi(object));
	}
}

/**
 * b1_uapi_peer_init() - XXX
 */
void b1_uapi_peer_init(struct b1_uapi_peer *peer)
{
	*peer = (struct b1_uapi_peer){};
	mutex_init(&peer->lock);
}

/**
 * b1_uapi_peer_deinit() - XXX
 */
void b1_uapi_peer_deinit(struct b1_uapi_peer *peer)
{
	B1_WARN_ON(!RB_EMPTY_ROOT(&peer->map_objects));
	B1_WARN_ON(!RB_EMPTY_ROOT(&peer->map_handles));
	mutex_destroy(&peer->lock);
}

/**
 * b1_uapi_new() - XXX
 */
struct b1_uapi_peer *b1_uapi_new(struct b1_acct_resource *res)
{
	struct b1_peer *p;

	p = b1_peer_new();
	if (IS_ERR(p))
		return ERR_CAST(p);

	/* XXX: @res ? */

	return &p->uapi;
}

/**
 * b1_uapi_free() - XXX
 */
struct b1_uapi_peer *b1_uapi_free(struct b1_uapi_peer *peer)
{
	if (!peer)
		return NULL;

	B1_WARN_ON(!RB_EMPTY_ROOT(&peer->map_handles));
	B1_WARN_ON(!RB_EMPTY_ROOT(&peer->map_objects));

	b1_peer_unref(b1_peer_from_uapi(peer));

	return NULL;
}

static struct b1_uapi_object*
b1_uapi_find_object_by_id(struct b1_uapi_peer *peer, u64 id)
{
	struct b1_uapi_object *object;
	struct rb_node *n;

	b1_assert_held(&peer->lock);

	n = peer->map_objects.rb_node;
	while (n) {
		object = container_of(n, struct b1_uapi_object, rb_peer);
		if (id < object->id)
			n = n->rb_left;
		else if (id > object->id)
			n = n->rb_right;
		else /* if (id == object->id) */
			return object;
	}

	return NULL;
}

static struct b1_uapi_handle*
b1_uapi_find_handle_by_id(struct b1_uapi_peer *peer, u64 id)
{
	struct b1_uapi_handle *handle;
	struct rb_node *n;

	b1_assert_held(&peer->lock);

	n = peer->map_handles.rb_node;
	while (n) {
		handle = container_of(n, struct b1_uapi_handle, rb_peer);
		if (id < handle->id)
			n = n->rb_left;
		else if (id > handle->id)
			n = n->rb_right;
		else /* if (id == handle->id) */
			return handle;
	}

	return NULL;
}

/**
 * b1_uapi_get_waitq() - XXX
 */
wait_queue_head_t *b1_uapi_get_waitq(struct b1_uapi_peer *peer)
{
	return &b1_peer_from_uapi(peer)->distq.waitq;
}

/**
 * b1_uapi_poll() - XXX
 */
unsigned int b1_uapi_poll(struct b1_uapi_peer *peer)
{
	unsigned int events = POLLOUT | POLLWRNORM;

	if (b1_distq_peer_poll(&b1_peer_from_uapi(peer)->distq))
		events |= EPOLLIN | POLLRDNORM;

	return events;
}

/**
 * b1_uapi_finalize() - XXX
 */
void b1_uapi_finalize(struct b1_uapi_peer *peer)
{
	struct b1_uapi_object *object, *t_object;
	struct b1_uapi_handle *handle, *t_handle;
	struct b1_stage stage;

	mutex_lock(&peer->lock);

	b1_stage_init(&stage, b1_peer_from_uapi(peer));

	/*
	 * First collect all objects and destroy them in a single transaction.
	 * This will completely shutdown the peer, as it will not have any
	 * valid target objects, anymore. Hence, we then continue by finalizing
	 * the queue (this flushes and seals the queue). Finally, we release
	 * all flushed queue-entries plus all owned handles in another single
	 * transaction.
	 */

	mutex_lock(&stage.peer->lock);
	rbtree_postorder_for_each_entry_safe(object, t_object,
					     &peer->map_objects, rb_peer) {
		b1_stage_add_destruction_locked(&stage,
						b1_object_from_uapi(object));
		RB_CLEAR_NODE(&object->rb_peer);
		b1_uapi_object_close(object);
	}
	peer->map_objects = RB_ROOT;
	mutex_unlock(&stage.peer->lock);

	b1_stage_commit(&stage);

	rbtree_postorder_for_each_entry_safe(handle, t_handle,
					     &peer->map_handles, rb_peer) {
		b1_stage_add_release(&stage, b1_handle_from_uapi(handle));
		RB_CLEAR_NODE(&handle->rb_peer);
		handle->n_public = 1;
		b1_uapi_handle_close(handle);
	}
	peer->map_handles = RB_ROOT;

	/* XXX: flush queue */

	b1_stage_commit(&stage);

	b1_stage_deinit(&stage);

	mutex_unlock(&peer->lock);
}

/**
 * b1_uapi_pair() - XXX
 */
int b1_uapi_pair(struct b1_uapi_peer *peer1,
		 struct b1_uapi_peer *peer2,
		 u64 flags,
		 u64 *object_idp,
		 u64 *handle_idp)
{
	struct b1_object *o = NULL;
	struct b1_handle *h = NULL;
	int r;

	/* XXX: accounting? */

	if (flags)
		return -EINVAL;

	b1_lock2(&peer1->lock, &peer2->lock);

	o = b1_object_new(b1_peer_from_uapi(peer1));
	if (IS_ERR(o)) {
		r = PTR_ERR(o);
		o = NULL;
		goto exit;
	}

	h = b1_handle_new(b1_peer_from_uapi(peer2), o);
	if (IS_ERR(h)) {
		r = PTR_ERR(h);
		h = NULL;
		goto exit;
	}

	b1_handle_launch(h);
	*object_idp = b1_uapi_object_open(&o->uapi);
	*handle_idp = b1_uapi_handle_open(&h->uapi);
	r = 0;

exit:
	b1_object_unref(o);
	b1_handle_unref(h);
	b1_unlock2(&peer1->lock, &peer2->lock);
	return r;
}

/**
 * b1_uapi_send() - XXX
 */
int b1_uapi_send(struct b1_uapi_peer *peer,
		 u64 flags,
		 u64 n_destinations,
		 const u64 __user *u_destinations,
		 const int __user *u_errors,
		 const struct bus1_message *message)
{
	const struct bus1_transfer __user *u_transfers;
	const struct iovec __user *u_data_vecs;
	struct b1_custom_stage stage;
	struct b1_uapi_handle *handle;
	u64 i, id;
	int r;

	u_transfers = (void __user *)(unsigned long)message->ptr_transfers;
	u_data_vecs = (void __user *)(unsigned long)message->ptr_data_vecs;

	if (flags ||
	    message->flags ||
	    message->type != BUS1_MESSAGE_TYPE_CUSTOM ||
	    message->ptr_transfers != (u64)u_transfers ||
	    message->n_data_vecs > UIO_MAXIOV ||
	    message->ptr_data_vecs != (u64)u_data_vecs)
		return -EINVAL;

	mutex_lock(&peer->lock);

	b1_custom_stage_init(&stage);

	r = b1_custom_stage_import(&stage,
				   message->n_transfers,
				   message->n_data,
				   message->n_data_vecs,
				   u_data_vecs);
	if (r < 0)
		goto exit;

	/* collect all the specified destinations */
	for (i = 0; i < n_destinations; ++i) {
		if (get_user(id, u_destinations + i)) {
			r = -EFAULT;
			goto exit;
		}

		handle = b1_uapi_find_handle_by_id(peer, id);
		if (!handle) {
			r = -EBADRQC;
			goto exit;
		}

		/* XXX */
	}

	r = -EIO; /* XXX */

exit:
	b1_custom_stage_deinit(&stage);
	mutex_unlock(&peer->lock);
	return r;
}

#if 0
static int b1_uapi_recv_message(struct b1_uapi_peer *peer,
				void *X, /* XXX */
				u64 *destinationp,
				struct bus1_message *message)
{
	/* XXX */
	return -EIO;
}

static int b1_uapi_recv_release(struct b1_uapi_peer *peer,
				void *X, /* XXX */
				u64 *destinationp,
				struct bus1_message *message)
{
	/* XXX */
	return -EIO;
}

static int b1_uapi_recv_destruction(struct b1_uapi_peer *peer,
				    void *X, /* XXX */
				    u64 *destinationp,
				    struct bus1_message *message)
{
	/* XXX */
	return -EIO;
}
#endif

/**
 * b1_uapi_recv() - XXX
 */
int b1_uapi_recv(struct b1_uapi_peer *peer,
		 u64 flags,
		 u64 *destinationp,
		 struct bus1_message *message)
{
	struct b1_peer *p = b1_peer_from_uapi(peer);
	//struct b1_event *event;
	//unsigned int type;
	int r;

	if (flags)
		return -EINVAL;

	mutex_lock(&peer->lock);

#if 0
	event = b1_peer_peek(p);
	if (!event) {
		r = -EAGAIN;
		goto exit;
	}

	type = b1_event_type(event);
	switch (type) {
	case B1_EVENT_MESSAGE:
		r = b1_uapi_recv_message(peer,
					 event,
					 destinationp,
					 message);
		break;
	case B1_EVENT_RELEASE:
		r = b1_uapi_recv_release(peer,
					 event,
					 destinationp,
					 message);
		break;
	case B1_EVENT_DESTRUCTION:
		r = b1_uapi_recv_destruction(peer,
					     event,
					     destinationp,
					     message);
		break;
	default:
		WARN_ONCE(1, "Invalid message type: %u\n", type);
		r = -ENOTRECOVERABLE;
		goto exit;
	}

	if (r >= 0)
		b1_peer_pop(p, event);
#else
	r = -EIO;
#endif

exit:
	mutex_unlock(&peer->lock);
	return r;
}

/**
 * b1_uapi_destroy() - XXX
 */
int b1_uapi_destroy(struct b1_uapi_peer *peer,
		    u64 flags,
		    u64 n_objects,
		    const u64 __user *u_objects)
{
	struct b1_uapi_object *object, *object_list = B1_TAIL;
	struct b1_stage stage;
	u64 i, id;
	int r;

	if (flags)
		return -EINVAL;

	mutex_lock(&peer->lock);

	b1_stage_init(&stage, b1_peer_from_uapi(peer));

	/* collect all the specified objects to destroy */
	for (i = 0; i < n_objects; ++i) {
		if (get_user(id, u_objects + i)) {
			r = -EFAULT;
			goto exit;
		}

		object = b1_uapi_find_object_by_id(peer, id);
		if (!object) {
			r = -EBADRQC;
			goto exit;
		}

		if (object->op.next) {
			r = -ENOTUNIQ;
			goto exit;
		}

		object->op.next = object_list;
		object_list = object;

		b1_stage_add_destruction(&stage, b1_object_from_uapi(object));
	}

	b1_stage_commit(&stage);

	while (object_list != B1_TAIL) {
		object = object_list;
		object_list = object->op.next;
		object->op.next = NULL;

		b1_uapi_object_close(object);
	}

	r = 0;

exit:
	while (object_list != B1_TAIL) {
		object = object_list;
		object_list = object->op.next;
		object->op.next = NULL;
	}
	b1_stage_deinit(&stage);
	mutex_unlock(&peer->lock);
	return r;
}

/**
 * b1_uapi_acquire() - XXX
 */
int b1_uapi_acquire(struct b1_uapi_peer *peer,
		    u64 flags,
		    u64 n_handles,
		    const u64 __user *u_handles)
{
	struct b1_uapi_handle *handle, *handle_list = B1_TAIL;
	u64 i, id;
	int r;

	if (flags)
		return -EINVAL;

	/* XXX: account @n_handles on @uapi to protect against exhaustion */

	mutex_lock(&peer->lock);

	/* collect all the specified handles */
	for (i = 0; i < n_handles; ++i) {
		if (get_user(id, u_handles + i)) {
			r = -EFAULT;
			goto exit;
		}

		handle = b1_uapi_find_handle_by_id(peer, id);
		if (!handle) {
			r = -EBADRQC;
			goto exit;
		}

		if (handle->op.next) {
			++handle->op.n;
		} else {
			handle->op.next = handle_list;
			handle_list = handle;
			handle->op.n = 1;
		}
	}

	/*
	 * Apply the ACQUIRE operation now that everything is verified. Note
	 * that `n_handles` is accounted, so this cannot overflow.
	 */
	for (handle = handle_list; handle != B1_TAIL; handle = handle->op.next)
		handle->n_public += handle->op.n;

	r = 0;

exit:
	while (handle_list != B1_TAIL) {
		handle = handle_list;
		handle_list = handle->op.next;
		handle->op.next = NULL;
		handle->op.n = 0;
	}
	mutex_unlock(&peer->lock);
	return r;
}

/**
 * b1_uapi_release() - XXX
 */
int b1_uapi_release(struct b1_uapi_peer *peer,
		    u64 flags,
		    u64 n_handles,
		    const u64 __user *u_handles)
{
	struct b1_uapi_handle *handle, *handle_list = B1_TAIL;
	u64 i, id;
	int r;

	if (flags & ~BUS1_RECV_FLAG_TRUNCATE)
		return -EINVAL;

	mutex_lock(&peer->lock);

	/* collect all the specified handles to release */
	for (i = 0; i < n_handles; ++i) {
		if (get_user(id, u_handles + i)) {
			r = -EFAULT;
			goto exit;
		}

		handle = b1_uapi_find_handle_by_id(peer, id);
		if (!handle) {
			r = -EBADRQC;
			goto exit;
		}

		if (!handle->op.next) {
			handle->op.next = handle_list;
			handle_list = handle;
			handle->op.n = 1;
		} else if (handle->op.n < handle->n_public) {
			++handle->op.n;
		} else {
			r = -EOVERFLOW;
			goto exit;
		}
	}

	/*
	 * With all the input validated, we can now actually commit this
	 * transaction and release all the specified handles atomically.
	 */

	r = -EIO; /* XXX */

exit:
	while (handle_list != B1_TAIL) {
		handle = handle_list;
		handle_list = handle->op.next;
		handle->op.next = NULL;
		handle->op.n = 0;
	}
	mutex_unlock(&peer->lock);
	return r;
}

// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include "core.h"
#include "uapi.h"
#include "util/distq.h"
#include "util/util.h"

void b1_message_warn_type(struct b1_message *message)
{
	WARN(1, "Invalid message type: %u\n", message->node.userdata);
}

static void b1_message_init(struct b1_message *message, unsigned int type)
{
	message->next = NULL;
	b1_distq_tx_init(&message->tx);
	b1_distq_node_init(&message->node);
	message->node.userdata = type;
}

static void b1_message_deinit(struct b1_message *message)
{
	B1_WARN_ON(message->next);
	b1_distq_node_deinit(&message->node);
	b1_distq_tx_deinit(&message->tx);
}

struct b1_message *b1_message_ref(struct b1_message *message)
{
	if (message) {
		switch (message->node.userdata) {
		case B1_MESSAGE_CUSTOM:
			/* XXX */
			break;
		case B1_MESSAGE_OBJECT_RELEASE:
			b1_object_ref(container_of(message, struct b1_object,
						   release));
			break;
		case B1_MESSAGE_HANDLE_RELEASE:
			b1_handle_ref(container_of(message, struct b1_handle,
						   release));
			break;
		case B1_MESSAGE_HANDLE_DESTRUCTION:
			b1_handle_ref(container_of(message, struct b1_handle,
						   destruction));
			break;
		default:
			b1_message_warn_type(message);
			break;
		}
	}

	return message;
}

struct b1_message *b1_message_unref(struct b1_message *message)
{
	if (message) {
		switch (message->node.userdata) {
		case B1_MESSAGE_CUSTOM:
			/* XXX */
			break;
		case B1_MESSAGE_OBJECT_RELEASE:
			b1_object_unref(container_of(message, struct b1_object,
						     release));
			break;
		case B1_MESSAGE_HANDLE_RELEASE:
			b1_handle_unref(container_of(message, struct b1_handle,
						     release));
			break;
		case B1_MESSAGE_HANDLE_DESTRUCTION:
			b1_handle_unref(container_of(message, struct b1_handle,
						     destruction));
			break;
		default:
			b1_message_warn_type(message);
			break;
		}
	}

	return NULL;
}

struct b1_handle *b1_handle_new(struct b1_peer *owner,
				struct b1_object *object)
{
	struct b1_handle *handle;

	handle = kmalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return ERR_PTR(-ENOMEM);

	kref_init(&handle->ref);
	handle->owner = b1_peer_ref(owner);
	handle->object = b1_object_ref(object);
	INIT_LIST_HEAD(&handle->link_object);
	b1_uapi_handle_init(&handle->uapi);
	b1_message_init(&handle->release, B1_MESSAGE_HANDLE_RELEASE);
	b1_message_init(&handle->destruction, B1_MESSAGE_HANDLE_DESTRUCTION);

	return handle;
}

/* internal callback for b1_handle_unref() */
void b1_handle_free_internal(struct kref *ref)
{
	struct b1_handle *handle = container_of(ref, struct b1_handle, ref);

	B1_WARN_ON(!list_empty(&handle->link_object));

	b1_message_deinit(&handle->destruction);
	b1_message_deinit(&handle->release);
	b1_uapi_handle_init(&handle->uapi);
	b1_object_unref(handle->object);
	b1_peer_unref(handle->owner);
	kfree(handle);
}

void b1_handle_launch(struct b1_handle *handle)
{
	mutex_lock(&handle->object->owner->lock);
	B1_WARN_ON(!list_empty(&handle->link_object));
	B1_WARN_ON(!list_empty(&handle->object->list_handles));
	list_add(&handle->link_object, &handle->object->list_handles);
	mutex_unlock(&handle->object->owner->lock);
}

struct b1_object *b1_object_new(struct b1_peer *owner)
{
	struct b1_object *object;

	object = kmalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return ERR_PTR(-ENOMEM);

	kref_init(&object->ref);
	object->owner = b1_peer_ref(owner);
	INIT_LIST_HEAD(&object->list_handles);
	b1_uapi_object_init(&object->uapi);
	b1_message_init(&object->release, B1_MESSAGE_OBJECT_RELEASE);

	return object;
}

/* internal callback for b1_object_unref() */
void b1_object_free_internal(struct kref *ref)
{
	struct b1_object *object = container_of(ref, struct b1_object, ref);

	B1_WARN_ON(!list_empty(&object->list_handles));

	b1_message_deinit(&object->release);
	b1_uapi_object_deinit(&object->uapi);
	b1_peer_unref(object->owner);
	kfree(object);
}

struct b1_peer *b1_peer_new(void)
{
	struct b1_peer *peer;

	peer = kmalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	kref_init(&peer->ref);
	mutex_init(&peer->lock);
	b1_distq_peer_init(&peer->distq);
	b1_uapi_peer_init(&peer->uapi);

	return peer;
}

/* internal callback for b1_peer_unref() */
void b1_peer_free_internal(struct kref *ref)
{
	struct b1_peer *peer = container_of(ref, struct b1_peer, ref);

	b1_uapi_peer_deinit(&peer->uapi);
	b1_distq_peer_deinit(&peer->distq);
	mutex_destroy(&peer->lock);
	kfree(peer);
}

void b1_stage_init(struct b1_stage *stage, struct b1_peer *peer)
{
	*stage = (struct b1_stage){
		.peer = b1_peer_ref(peer),
		.list = B1_TAIL,
	};
}

void b1_stage_deinit(struct b1_stage *stage)
{
	B1_WARN_ON(stage->list != B1_TAIL);
	stage->peer = b1_peer_unref(stage->peer);
}

void b1_stage_add_destruction_locked(struct b1_stage *stage,
				     struct b1_object *o)
{
	struct b1_handle *h, *safe;

	if (WARN_ON(o->release.next || o->owner != stage->peer))
		return;

	b1_assert_held(&o->owner->lock);

	o->release.next = stage->list;
	stage->list = &o->release;
	b1_object_ref(o);

	/*
	 * We collect all destruction events from all registered handles. At
	 * the same time, we clear them from the list to tell racing handle
	 * transfers about this ongoing destruction. If a handle transfer sees
	 * the empty handle-list, it will correctly insert the destruction
	 * event in its own queue.
	 */
	list_for_each_entry_safe(h, safe, &o->list_handles, link_object) {
		list_del_init(&h->link_object);
		h->destruction.next = stage->list;
		stage->list = &h->destruction;
		b1_handle_ref(h);
	}
}

void b1_stage_add_destruction(struct b1_stage *stage, struct b1_object *o)
{
	mutex_lock(&o->owner->lock);
	b1_stage_add_destruction_locked(stage, o);
	mutex_unlock(&o->owner->lock);
}

void b1_stage_add_release_locked(struct b1_stage *stage, struct b1_handle *h)
{
	if (WARN_ON(h->release.next))
		return;

	b1_assert_held(&h->object->owner->lock);

	/*
	 * Releasing the handle means it will vanish from the namespace of its
	 * owner. Therefore, once the RELEASE operation finishes, the handle
	 * owner will no longer see any messages on that handle. We can thus
	 * immediately unlink it from its object. This might cause us to miss
	 * racing destruction events, but that does not matter since we would
	 * flush them once this RELEASE is committed, anyway.
	 */
	if (!list_empty(&h->link_object)) {
		list_del_init(&h->link_object);

		h->release.next = stage->list;
		stage->list = &h->release;
		b1_handle_ref(h);
	}
}

void b1_stage_add_release(struct b1_stage *stage, struct b1_handle *h)
{
	mutex_lock(&h->object->owner->lock);
	b1_stage_add_release_locked(stage, h);
	mutex_unlock(&h->object->owner->lock);
}

static void b1_stage_submit(struct b1_stage *stage, struct b1_distq_tx *tx)
{
	struct b1_message *m;
	struct b1_object *o;
	struct b1_handle *h;

	for (m = stage->list; m != B1_TAIL; m = m->next) {
		switch (m->node.userdata) {
		case B1_MESSAGE_CUSTOM:
			/* XXX */
			break;
		case B1_MESSAGE_OBJECT_RELEASE:
			o = container_of(m, struct b1_object, release);
			b1_distq_node_claim(&o->release.node);
			b1_distq_node_queue(&o->release.node, tx,
					    &o->owner->distq);
			break;
		case B1_MESSAGE_HANDLE_RELEASE:
			h = container_of(m, struct b1_handle, release);
			b1_distq_node_claim(&h->release.node);
			b1_distq_node_queue(&h->release.node, tx,
					    &h->object->owner->distq);
			break;
		case B1_MESSAGE_HANDLE_DESTRUCTION:
			h = container_of(m, struct b1_handle, destruction);
			b1_distq_node_claim(&h->destruction.node);
			b1_distq_node_queue(&h->destruction.node, tx,
					    &h->owner->distq);
			break;
		default:
			b1_message_warn_type(m);
			continue;
		}
	}
}

static void b1_stage_settle(struct b1_stage *stage, struct b1_distq_tx *tx)
{
	struct b1_message *m, *cleanup = B1_TAIL;
	struct b1_object *o;
	struct b1_handle *h;

	b1_distq_tx_commit(tx, &stage->peer->distq);

	while ((m = stage->list) != B1_TAIL) {
		stage->list = m->next;
		m->next = NULL;

		switch (m->node.userdata) {
		case B1_MESSAGE_CUSTOM:
			/* XXX */
			break;
		case B1_MESSAGE_OBJECT_RELEASE:
			o = container_of(m, struct b1_object, release);
			b1_distq_node_commit(&m->node, &o->owner->distq);
			b1_message_unref_node(&m->node);
			break;
		case B1_MESSAGE_HANDLE_RELEASE:
			h = container_of(m, struct b1_handle, release);
			b1_distq_node_commit(&m->node,
					     &h->object->owner->distq);
			b1_message_unref_node(&m->node);
			break;
		case B1_MESSAGE_HANDLE_DESTRUCTION:
			h = container_of(m, struct b1_handle, destruction);
			b1_distq_node_commit(&m->node, &h->owner->distq);
			b1_message_unref_node(&m->node);
			break;
		default:
			b1_message_warn_type(m);
			continue;
		}
	}
	stage->list = cleanup;
}

static void b1_stage_cleanup(struct b1_stage *stage)
{
	struct b1_message *m;

	while ((m = stage->list) != B1_TAIL) {
		stage->list = m->next;
		m->next = NULL;

		if (B1_WARN_ON(m->node.userdata != B1_MESSAGE_CUSTOM))
			continue;

		/* XXX */
	}
}

void b1_stage_commit(struct b1_stage *stage)
{
	struct b1_distq_tx *tx;

	if (stage->list == B1_TAIL)
		return;

	/*
	 * Rather than dynamically allocating the transaction contexts, they
	 * are statically embedded in every node. We simply claim the context
	 * of the first entry and use it for the transaction.
	 */
	b1_message_ref(stage->list);
	tx = &stage->list->tx;
	b1_distq_tx_claim(tx);

	/*
	 * XXX
	 */
	preempt_disable();
	b1_stage_submit(stage, tx);
	b1_stage_settle(stage, tx);
	preempt_enable();

	b1_stage_cleanup(stage);
	b1_message_unref_tx(tx);
}

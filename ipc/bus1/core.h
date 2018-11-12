// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_CORE_H
#define __B1_CORE_H

/**
 * DOC: Kernel-internal Bus1 API
 *
 * XXX
 */

#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include "uapi.h"
#include "util/distq.h"

struct b1_handle;
struct b1_message;
struct b1_object;
struct b1_peer;
struct b1_stage;

enum {
	B1_MESSAGE_CUSTOM,
	B1_MESSAGE_OBJECT_RELEASE,
	B1_MESSAGE_HANDLE_RELEASE,
	B1_MESSAGE_HANDLE_DESTRUCTION,
	_B1_MESSAGE_N,
};

struct b1_message {
	struct b1_message *next;
	struct b1_distq_tx tx;
	struct b1_distq_node node;
};

struct b1_handle {
	struct kref ref;
	struct b1_peer *owner;
	struct b1_object *object;
	struct list_head link_object;
	struct b1_uapi_handle uapi;
	struct b1_message release;
	struct b1_message destruction;
};

struct b1_object {
	struct kref ref;
	struct b1_peer *owner;
	struct list_head list_handles;
	struct b1_uapi_object uapi;
	struct b1_message release;
};

struct b1_peer {
	struct kref ref;
	struct mutex lock;
	struct b1_distq_peer distq;
	struct b1_uapi_peer uapi;
};

struct b1_stage {
	struct b1_peer *peer;
	struct b1_message *list;
};

/* messages */

void b1_message_warn_type(struct b1_message *message);
struct b1_message *b1_message_ref(struct b1_message *message);
struct b1_message *b1_message_unref(struct b1_message *message);

/* handles */

struct b1_handle *b1_handle_new(struct b1_peer *owner,
				struct b1_object *object);
void b1_handle_free_internal(struct kref *ref);

void b1_handle_launch(struct b1_handle *handle);

/* objects */

struct b1_object *b1_object_new(struct b1_peer *owner);
void b1_object_free_internal(struct kref *ref);

/* peers */

struct b1_peer *b1_peer_new(void);
void b1_peer_free_internal(struct kref *ref);

struct b1_event *b1_peer_peek(struct b1_peer *peer);
void b1_peer_pop(struct b1_peer *peer, struct b1_event *event);

/* stage */

void b1_stage_init(struct b1_stage *stage, struct b1_peer *peer);
void b1_stage_deinit(struct b1_stage *stage);

void b1_stage_add_destruction_locked(struct b1_stage *stage,
				     struct b1_object *o);
void b1_stage_add_destruction(struct b1_stage *stage, struct b1_object *o);
void b1_stage_add_release_locked(struct b1_stage *stage, struct b1_handle *h);
void b1_stage_add_release(struct b1_stage *stage, struct b1_handle *h);
void b1_stage_commit(struct b1_stage *stage);

/* inline helpers */

static inline struct b1_distq_tx *
b1_message_unref_tx(struct b1_distq_tx *tx)
{
	if (tx && refcount_dec_and_test(&tx->n_refs))
		b1_message_unref(container_of(tx, struct b1_message, tx));
	return NULL;
}

static inline struct b1_distq_node *
b1_message_unref_node(struct b1_distq_node *node)
{
	if (node && refcount_dec_and_test(&node->n_refs)) {
		b1_message_unref_tx(b1_distq_node_finalize(node));
		b1_message_unref(container_of(node, struct b1_message, node));
	}
	return NULL;
}

static inline struct b1_handle *b1_handle_ref(struct b1_handle *handle)
{
	if (handle)
		kref_get(&handle->ref);
	return handle;
}

static inline struct b1_handle *b1_handle_unref(struct b1_handle *handle)
{
	if (handle)
		kref_put(&handle->ref, b1_handle_free_internal);
	return NULL;
}

static inline struct b1_handle *
b1_handle_from_uapi(struct b1_uapi_handle *handle)
{
	return handle ? container_of(handle, struct b1_handle, uapi) : NULL;
}

static inline struct b1_object *b1_object_ref(struct b1_object *object)
{
	if (object)
		kref_get(&object->ref);
	return object;
}

static inline struct b1_object *b1_object_unref(struct b1_object *object)
{
	if (object)
		kref_put(&object->ref, b1_object_free_internal);
	return NULL;
}

static inline struct b1_object *
b1_object_from_uapi(struct b1_uapi_object *object)
{
	return object ? container_of(object, struct b1_object, uapi) : NULL;
}

static inline struct b1_peer *b1_peer_ref(struct b1_peer *peer)
{
	if (peer)
		kref_get(&peer->ref);
	return peer;
}

static inline struct b1_peer *b1_peer_unref(struct b1_peer *peer)
{
	if (peer)
		kref_put(&peer->ref, b1_peer_free_internal);
	return NULL;
}

static inline struct b1_peer *
b1_peer_from_uapi(struct b1_uapi_peer *peer)
{
	return peer ? container_of(peer, struct b1_peer, uapi) : NULL;
}

#endif /* __B1_CORE_H */
